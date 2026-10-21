#include "StreamingValidation.h"

#include "Runtime/Core/Log/Log.h"
#include "Runtime/Core/Window/Window.h"
#include "Runtime/Graphics/RHI/RenderDevice.h"
#include "Runtime/Graphics/RHI/BindlessDescriptorManager.h"
#include "Runtime/Graphics/RHI/TemporalUpscaler.h"
#include "Runtime/Graphics/RHI/TextureReadback.h"
#include "Runtime/Graphics/RenderGraph/RenderGraphResourcePool.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <HEngine/ProjectBuildConfig.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace
{
    void Require(bool success, const std::string& error)
    {
        if (!success) throw std::runtime_error(error);
    }

    void VerifyOutput(const std::filesystem::path& path, uint32_t width, uint32_t height)
    {
        std::ifstream input(path, std::ios::binary);
        std::string magic;
        uint32_t actualWidth = 0, actualHeight = 0;
        float scale = 0.0f;
        input >> magic >> actualWidth >> actualHeight >> scale;
        Require(magic == "PF" && actualWidth == width && actualHeight == height && scale == -1.0f,
            "Readback header/dimensions are incorrect");
        input.get();
        std::vector<float> pixels(size_t(width) * height * 3);
        input.read(reinterpret_cast<char*>(pixels.data()), std::streamsize(pixels.size() * sizeof(float)));
        Require(bool(input), "Truncated FSR readback");
        constexpr std::array expected{0.25f, 0.5f, 1.0f};
        float maximumError = 0.0f;
        for (size_t index = 0; index < pixels.size(); ++index)
        {
            Require(std::isfinite(pixels[index]), "Nonfinite FSR output");
            maximumError = std::max(maximumError, std::abs(pixels[index] - expected[index % 3]));
        }
        Require(maximumError < 0.01f, "FSR output did not preserve constant scene-linear color");
        std::cout << "FSR_READBACK_MAX_ERROR " << maximumError << std::endl;
    }

    void VerifyPreparedBanks(nvrhi::IDevice* device)
    {
        using namespace HEngine;
        RenderGraphResourcePool pool(device, 3);
        pool.SetTransientCacheBudget(0);
        RenderGraph graph;
        auto texture = graph.CreateTexture({.DebugName = "Prepared bank trim regression",
            .Width = 32, .Height = 32, .Format = nvrhi::Format::RGBA8_UNORM,
            .CapabilityUsage = RenderGraphUsage::ShaderResource});
        struct PassData {};
        graph.AddPass<PassData>("PrepareOnly",
            [&texture](RenderGraphBuilder& builder, PassData&) {
                texture = builder.RenderTarget(texture);
                builder.SetSideEffect();
            }, [](RenderGraphContext&, const PassData&) {});
        std::array<nvrhi::TextureHandle, 3> prepared;
        for (uint32_t slot = 0; slot < 3; ++slot)
        {
            pool.BeginFrame(slot, 0);
            graph.PrepareResources(pool, slot);
            prepared[slot] = graph.GetPhysicalTexture(texture);
            pool.EndPreparation(slot);
        }
        pool.Collect(0);
        Require(pool.GetStats().TransientTextureCount == 3 && pool.GetStats().TrimmedTransientBytes == 0,
            "Completed preparation lost a bank to the zero-byte cache budget");
        for (uint32_t slot = 0; slot < 3; ++slot)
        {
            pool.BeginFrame(slot, 0);
            graph.PrepareResources(pool, slot);
            Require(graph.GetPhysicalTexture(texture) == prepared[slot],
                "A prepared bank required a replacement allocation on first use");
            pool.EndPreparation(slot);
            Require(pool.GetFrameSlotState(slot).LastSubmission == 0,
                "CPU preparation fabricated a GPU submission");
        }
        pool.AbortFrame(1);
        pool.Collect(0);
        Require(pool.GetStats().TransientTextureCount == 2,
            "Explicit abort no longer makes unused preparation reclaimable");
        std::cout << "FSR_PREPARED_BANKS_PASS: three banks retained and reused under zero cache budget\n";
    }

    void VerifyTexturePublication(nvrhi::IDevice* device, HEngine::RenderBackend backend)
    {
        using namespace HEngine;
        nvrhi::TextureDesc desc;
        desc.width = desc.height = 4;
        desc.format = nvrhi::Format::RGBA8_UNORM;
        desc.isShaderResource = true;
        auto first = device->createTexture(desc);
        auto second = device->createTexture(desc);
        Require(first && second, "Publication fixture textures could not be allocated");
        {
            BindlessTextureTable table(device, {.Capacity = 5});
            table.SetDefaultTexture(DefaultBindlessTexture::White, first);
            auto handle = table.AllocateTexture(first);
            const auto original = handle;
            nvrhi::BindingSetHandle previousSet = table.GetBindingSet();
            Require(table.TryPublishTexture(handle, second, 7, 0), "Bounded publication unexpectedly deferred");
            nvrhi::BindingSetHandle currentSet = table.GetBindingSet();
            Require(handle == original && previousSet != currentSet &&
                previousSet->getDesc()->bindings[handle.Index].resourceHandle == first.Get() &&
                currentSet->getDesc()->bindings[handle.Index].resourceHandle == second.Get(),
                "Bounded publication mutated the prior immutable binding set");
            table.ReleaseTexture(handle, 7);
            table.Collect(6);
            Require(table.GetRetiredTextureCount() == 1, "Bounded texture retired before its submission");
            table.Collect(7);
            Require(table.GetRetiredTextureCount() == 0 &&
                previousSet->getDesc()->bindings[original.Index].resourceHandle == first.Get(),
                "Retained bounded snapshot lost its original texture");
        }
        if (backend == RenderBackend::DX12)
        {
            BindlessTextureTable table(device, {.Capacity = 5, .Access = ShaderDescriptorAccess::HeapDirectlyIndexed});
            auto handle = table.AllocateTexture(first);
            const auto original = handle;
            const auto originalIndex = table.GetShaderDescriptorIndex(handle);
            Require(table.TryPublishTexture(handle, second, 7, 0) && handle != original &&
                table.GetShaderDescriptorIndex(handle) != originalIndex && !table.IsValid(original) &&
                table.GetRetiredTextureCount() == 1,
                "Heap publication overwrote a descriptor that still has readers");
            const auto published = handle;
            Require(!table.TryPublishTexture(handle, first, 8, 0) && handle == published,
                "Exhausted heap failed to defer publication without mutation");
            table.Collect(6);
            Require(!table.TryPublishTexture(handle, first, 8, 6), "Old descriptor recycled before its fence");
            table.Collect(7);
            Require(table.TryPublishTexture(handle, first, 8, 7) && handle.Index == original.Index &&
                handle.Generation != original.Generation && table.GetRetiredTextureCount() == 1,
                "Completed descriptor was not recycled with a new generation");
            table.Collect(8);
            const auto spare = table.AllocateTexture(second);
            const auto idleHandle = handle;
            Require(!table.TryPublishTexture(handle, second, 9, 8) &&
                table.TryPublishTexture(handle, second, 9, 9) && handle == idleHandle,
                "A full heap must defer in flight but permit overwrite once all readers complete");
            table.ReleaseTexture(handle, 9);
            table.ReleaseTexture(spare, 9);
            table.Collect(9);
            Require(table.GetLiveTextureCount() == 0 && table.GetRetiredTextureCount() == 0,
                "Publication leaked live or retired descriptor slots");
        }
        std::cout << "TEXTURE_PUBLICATION_PASS: bounded snapshots and heap descriptor retirement\n";
    }

    void RunBackend(HEngine::RenderBackend backend, const std::filesystem::path& evidence, bool bUseWarp, bool bCompact)
    {
        using namespace HEngine;
        auto window = Window::Create(WindowProps("FSR 3.1 bounded GPU smoke", 640, 360,
            false, true, false, false, false));
        window->SetEventCallback([](Event&) {});
        auto* surface = dynamic_cast<RenderSurface*>(window.get());
        Require(surface != nullptr, "Window has no render surface");
        RenderDevice renderer;
        renderer.Initialize(backend, *surface, !bUseWarp, {.bUseWarp = bUseWarp});
        auto* device = renderer.GetDevice();
        std::unique_ptr<ITemporalUpscaler> upscaler;
        try
        {
            VerifyTexturePublication(device, backend);
#if HE_FSR_STREAMING_VALIDATION
            if (!bCompact) VerifyTextureStreaming(device, backend, evidence);
#endif
            VerifyPreparedBanks(device);
            std::string error;
            Require(!CreateFsr31Upscaler(device, {}, error) && !error.empty(), "Invalid context was accepted");
            const std::vector<std::array<uint32_t, 2>> outputSizes = bCompact
                ? std::vector<std::array<uint32_t, 2>>{{640, 360}, {800, 450}}
                : std::vector<std::array<uint32_t, 2>>{{1920, 1080}, {2560, 1440}, {3840, 2160}};
            for (const auto outputSize : outputSizes)
            {
                const uint32_t width = outputSize[0] / 2;
                const uint32_t height = outputSize[1] / 2;
                TemporalUpscalerDesc create;
                create.RenderWidth = width;
                create.RenderHeight = height;
                create.OutputWidth = outputSize[0];
                create.OutputHeight = outputSize[1];
                upscaler = CreateFsr31Upscaler(device, create, error);
                Require(upscaler != nullptr, error);
                std::cout << "FSR_PROVIDER " << ToString(backend) << " " << upscaler->GetVersion()
                    << " " << width << "x" << height << " -> " << outputSize[0] << "x" << outputSize[1] << std::endl;
                auto texture = [&](nvrhi::Format format, bool output = false)
                {
                    nvrhi::TextureDesc desc;
                    desc.width = output ? outputSize[0] : width;
                    desc.height = output ? outputSize[1] : height;
                    desc.format = format;
                    desc.isUAV = true;
                    desc.initialState = nvrhi::ResourceStates::ShaderResource;
                    desc.keepInitialState = true;
                    desc.debugName = "FSR smoke";
                    auto result = device->createTexture(desc);
                    Require(result != nullptr, "Cannot create smoke texture");
                    return result;
                };
                auto color = texture(nvrhi::Format::RGBA32_FLOAT);
                auto depth = texture(nvrhi::Format::R32_FLOAT);
                auto motion = texture(nvrhi::Format::RG16_FLOAT);
                auto reactive = texture(nvrhi::Format::R8_UNORM);
                auto output = texture(nvrhi::Format::RGBA32_FLOAT, true);
                TextureReadbackQueue readback(device);
                TextureReadbackHandle capture;
                uint64_t submission = 0;
                for (uint32_t index = 0; index < 8; ++index)
                {
                    window->OnUpdate();
                    auto& frame = renderer.BeginFrame();
                    auto* commands = frame.GraphicsCommandList.Get();
                    try
                    {
                        commands->clearTextureFloat(color, nvrhi::AllSubresources, nvrhi::Color(0.25f, 0.5f, 1.0f, 1.0f));
                        commands->clearTextureFloat(depth, nvrhi::AllSubresources, nvrhi::Color(0.5f));
                        commands->clearTextureFloat(motion, nvrhi::AllSubresources, nvrhi::Color(0.0f));
                        commands->clearTextureFloat(reactive, nvrhi::AllSubresources, nvrhi::Color(index == 4 ? 1.0f : 0.0f));
                        TemporalUpscalerFrame dispatch;
                        dispatch.CommandList = commands;
                        dispatch.Color = color;
                        dispatch.Depth = depth;
                        dispatch.MotionVectors = motion;
                        dispatch.Reactive = reactive;
                        dispatch.Composition = reactive;
                        dispatch.Output = output;
                        dispatch.RenderWidth = width;
                        dispatch.RenderHeight = height;
                        dispatch.MotionScaleX = float(width);
                        dispatch.MotionScaleY = float(height);
                        dispatch.Sharpness = index >= 4 ? 0.3f : 0.0f;
                        dispatch.bReset = index == 0 || index == 4;
                        Require(upscaler->GetJitter(index, width, dispatch.JitterX, dispatch.JitterY, error), error);
                        Require(upscaler->Dispatch(dispatch, error), error);
                        if (index == 7)
                        {
                            TextureReadbackDesc read;
                            read.AlphaMode = TextureReadbackAlphaMode::Discard;
                            read.OutputPath = evidence / (std::string(ToString(backend)) + "-" + std::to_string(outputSize[1]) + ".pfm");
                            capture = readback.Record(commands, output, read);
                        }
                        // NVRHI commands after native FSR exercise restoration
                        // of pipeline/descriptor state and resource transitions.
                        auto& swapChain = renderer.GetSwapChain();
                        auto* backBuffer = swapChain.GetCurrentBackBuffer();
                        commands->beginTrackingTextureState(backBuffer, nvrhi::AllSubresources,
                            swapChain.GetCurrentBackBufferInitialState());
                        commands->clearTextureFloat(backBuffer, nvrhi::AllSubresources, nvrhi::Color(0.1f));
                        commands->setTextureState(backBuffer, nvrhi::AllSubresources, nvrhi::ResourceStates::Present);
                        commands->commitBarriers();
                        renderer.SubmitAndPresent(frame, false);
                        submission = frame.SubmissionInstance;
                        if (capture) readback.NotifySubmitted(capture, submission);
                    }
                    catch (...)
                    {
                        renderer.AbortFrame(frame);
                        throw;
                    }
                }
                renderer.WaitForIdle();
                readback.Drain(submission);
                const auto result = readback.GetResult(capture);
                Require(result.Status == TextureReadbackStatus::Complete, result.Error);
                VerifyOutput(result.OutputPath, outputSize[0], outputSize[1]);
                TemporalUpscalerFrame invalid;
                Require(!upscaler->Dispatch(invalid, error) && !error.empty(), "Invalid dispatch was accepted");
                upscaler.reset();
                window->SetClientSize(800, 450);
                window->OnUpdate();
                renderer.Resize(800, 450);
                Require(renderer.GetSwapChain().GetWidth() == 800 && renderer.GetSwapChain().GetHeight() == 450,
                    "Swap-chain resize did not match the window extent");
                std::cout << "FSR_SMOKE_STAGE_PASS " << ToString(backend) << " " << outputSize[1] << std::endl;
            }
            renderer.Shutdown();
        }
        catch (...)
        {
            renderer.WaitForIdle();
            upscaler.reset();
            renderer.Shutdown();
            throw;
        }
    }
}

int main(int argc, char** argv)
{
    HEngine::Log::Init();
    try
    {
        std::filesystem::path evidence = "FsrSmokeEvidence";
        std::filesystem::path projectRoot = HE_DEFAULT_PROJECT_DIR;
        std::filesystem::path engineContent = HE_ENGINE_CONTENT_DIR;
        std::filesystem::path generatedContent = HE_ENGINE_GENERATED_CONTENT_DIR;
        bool bUseWarp = false;
        bool bDx12Only = false;
        bool bCompact = false;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument(argv[index]);
            if (argument.starts_with("--saved-root="))
                evidence = std::filesystem::path(argument.substr(13)) / "Diagnostics/FsrSmoke";
            else if (argument.starts_with("--fsr-evidence="))
                evidence = std::filesystem::path(argument.substr(15));
            else if (argument.starts_with("--project=")) projectRoot = argument.substr(10);
            else if (argument.starts_with("--engine-content=")) engineContent = argument.substr(17);
            else if (argument.starts_with("--generated-content=")) generatedContent = argument.substr(20);
            else if (argument == "--warp") bUseWarp = true;
            else if (argument == "--dx12-only") bDx12Only = true;
            else if (argument == "--compact") bCompact = true;
        }
        Require(!bUseWarp || bDx12Only, "WARP smoke requires --dx12-only");
        HEngine::ConfigManager::GetInstance().Init(projectRoot, engineContent, {}, generatedContent, HE_PROJECT_GAME_CONFIG);
#if !HE_RENDER_DX12
        Require(!bDx12Only, "DX12 was not compiled");
#endif
        evidence = std::filesystem::absolute(evidence);
        std::filesystem::create_directories(evidence);
#if HE_RENDER_DX12
        RunBackend(HEngine::RenderBackend::DX12, evidence, bUseWarp, bCompact);
#endif
#if HE_RENDER_VULKAN
        if (!bDx12Only) RunBackend(HEngine::RenderBackend::Vulkan, evidence, false, bCompact);
#endif
        std::cout << "FSR_SMOKE_PASS: native dispatch, readback, reset, resize, invalid input rejection\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FSR_SMOKE_FAIL: " << error.what() << '\n';
        return 1;
    }
}
