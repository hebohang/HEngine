#include "Runtime/Core/Log/Log.h"
#include "Runtime/Core/Window/Window.h"
#include "Runtime/Graphics/RHI/RenderDevice.h"
#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include "Runtime/Graphics/RenderGraph/RenderGraphResourcePool.h"
#include "Runtime/Graphics/VFX/EffekseerRenderGraphPass.h"
#include "Runtime/Graphics/VFX/EffekseerRuntime.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <HEngine/ProjectBuildConfig.h>

#include <EffekseerRendererCommon/EffekseerRenderer.Renderer.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    constexpr int c_Width = 640;
    constexpr int c_Height = 360;
    constexpr int c_ActiveDrawFrames = 60;
    constexpr const char* c_CpuEffectPath =
        "Assets/TestData/Smoke/Effekseer/HModify_Simple_Ring_Shape2.efkpkg";
    constexpr const char* c_GpuEffectPath =
        "Assets/TestData/Smoke/Effekseer/GpuSparks20k.efkpkg";

    struct SceneSetupPassData
    {
        HEngine::TextureHandle SceneColor;
        HEngine::TextureHandle SceneDepth;
    };

    struct ConsumePassData
    {
        HEngine::TextureHandle SceneColor;
    };

    [[nodiscard]] HEngine::RenderGraphTextureDesc MakeTextureDesc(
        const char* debugName,
        nvrhi::Format format)
    {
        HEngine::RenderGraphTextureDesc desc;
        desc.DebugName = debugName;
        desc.Width = c_Width;
        desc.Height = c_Height;
        desc.Format = format;
        desc.Dimension = nvrhi::TextureDimension::Texture2D;
        desc.bUseClearValue = true;
        desc.ClearValue = format == nvrhi::Format::D32
            ? nvrhi::Color(1.0f, 0.0f, 0.0f, 0.0f)
            : nvrhi::Color(0.025f, 0.04f, 0.08f, 1.0f);
        return desc;
    }

    class EffekseerGraphSmoke final
    {
    public:
        EffekseerGraphSmoke(
            nvrhi::IDevice* device,
            uint32_t framesInFlight,
            const std::filesystem::path& shaderCacheDirectory)
            : mResourcePool(device, framesInFlight)
            , mVfxPass({
                .Device = device,
                .ShaderCacheDirectory = shaderCacheDirectory,
            })
        {
            mGraph.Resize(c_Width, c_Height);
            mGraph.AddPass<SceneSetupPassData>(
                "VFXSmokeSceneSetup",
                [this](HEngine::RenderGraphBuilder& builder, SceneSetupPassData& data)
                {
                    data.SceneColor = builder.CreateTexture(MakeTextureDesc(
                        "VFX Smoke HDR Scene Color",
                        nvrhi::Format::RGBA16_FLOAT));
                    data.SceneDepth = builder.CreateTexture(MakeTextureDesc(
                        "VFX Smoke Shader-Readable Depth",
                        nvrhi::Format::D32));
                    data.SceneColor = builder.RenderTarget(data.SceneColor);
                    data.SceneDepth = builder.DepthWrite(data.SceneDepth);
                    mSceneColor = data.SceneColor;
                    mSceneDepth = data.SceneDepth;
                },
                [](HEngine::RenderGraphContext& context, const SceneSetupPassData& data)
                {
                    context.GetCommandList()->clearTextureFloat(
                        context.GetTexture(data.SceneColor, HEngine::RenderGraphAccess::Write),
                        nvrhi::AllSubresources,
                        nvrhi::Color(0.025f, 0.04f, 0.08f, 1.f));
                    context.GetCommandList()->clearDepthStencilTexture(
                        context.GetTexture(data.SceneDepth, HEngine::RenderGraphAccess::Write),
                        nvrhi::AllSubresources,
                        true,
                        1.0f,
                        false,
                        0);
                });

            // Keep both optional resource paths active while the regression
            // effect records real CPU-sprite draws through the native bridge.
            const glm::vec3 cameraPosition(0.0f, 0.0f, 5.0f);
            const glm::mat4 view = glm::lookAt(
                cameraPosition,
                glm::vec3(0.0f),
                glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::mat4 projection = glm::perspective(
                glm::radians(60.0f),
                static_cast<float>(c_Width) / static_cast<float>(c_Height),
                0.1f,
                1000.0f);
            const HEngine::EffekseerRenderGraphPassResult vfxResult = mVfxPass.AddToGraph(
                mGraph,
                HEngine::EffekseerRenderGraphInputs{
                    .SceneColor = mSceneColor,
                    .SceneDepth = mSceneDepth,
                    .Width = c_Width,
                    .Height = c_Height,
                    .View = view,
                    .Projection = projection,
                    .CameraPosition = cameraPosition,
                    .ZNear = 0.1f,
                    .ZFar = 1000.0f,
                },
                HEngine::VFXRenderRequirements{
                    .bHasActiveEffects = true,
                    .bRequiresDepthSnapshot = true,
                    .bRequiresBackgroundSnapshot = true,
                },
                true);
            if (!vfxResult.bAdded || !vfxResult.bDepthSnapshotAdded ||
                !vfxResult.bBackgroundCopyAdded)
            {
                throw std::runtime_error(
                    "Effekseer graph smoke did not declare every required optional resource path");
            }

            mGraph.AddPass<ConsumePassData>(
                "VFXSmokeConsume",
                [this](HEngine::RenderGraphBuilder& builder, ConsumePassData& data)
                {
                    data.SceneColor = builder.ShaderResource(mSceneColor);
                    builder.SetSideEffect();
                },
                [](HEngine::RenderGraphContext& context, const ConsumePassData& data)
                {
                    (void)context.GetTexture(data.SceneColor, HEngine::RenderGraphAccess::Read);
                });

            mGraph.CompileOrThrow();
            const std::vector<std::string> expectedPasses = {
                "VFXSmokeSceneSetup",
                "VFXDepthSnapshot",
                "VFXBackgroundCopy",
                "VFXTransparent",
                "VFXSmokeConsume",
            };
            if (mGraph.GetCompiledPassNames() != expectedPasses)
                throw std::runtime_error("Effekseer graph smoke compiled an unexpected pass order");
        }

        void RecordFrame(const HEngine::FrameContext& frame)
        {
            mResourcePool.Collect(frame.CompletedSubmissionInstance);
            mResourcePool.BeginFrame(frame.FrameIndex, frame.CompletedSubmissionInstance);
            try
            {
                mGraph.Execute(HEngine::RenderGraphExecutionParams{
                    .CommandList = frame.GraphicsCommandList,
                    .ResourcePool = &mResourcePool,
                    .FrameSlot = frame.FrameIndex,
                });
            }
            catch (...)
            {
                mResourcePool.AbortFrame(frame.FrameIndex);
                throw;
            }
        }

        void NotifySubmitted(uint32_t frameSlot, uint64_t submission)
        {
            mResourcePool.EndFrame(frameSlot, submission);
        }

        void AbortFrame(uint32_t frameSlot) noexcept
        {
            mResourcePool.AbortFrame(frameSlot);
        }

    private:
        HEngine::RenderGraphResourcePool mResourcePool;
        HEngine::RenderGraph mGraph;
        HEngine::EffekseerRenderGraphPass mVfxPass;
        HEngine::TextureHandle mSceneColor;
        HEngine::TextureHandle mSceneDepth;
    };

    void RunInteropFrame(
        HEngine::Window& window,
        HEngine::RenderDevice& renderDevice,
        EffekseerGraphSmoke& graphSmoke)
    {
        window.OnUpdate();
        HEngine::FrameContext& frame = renderDevice.BeginFrame();
        HEngine::SwapChain& swapChain = renderDevice.GetSwapChain();
        frame.GraphicsCommandList->clearTextureFloat(
            swapChain.GetCurrentBackBuffer(),
            nvrhi::AllSubresources,
            nvrhi::Color(0.025f, 0.04f, 0.08f, 1.f));

        HEngine::EffekseerRuntime::Update(HEngine::Timestep(1.0f / 60.0f));
        try
        {
            graphSmoke.RecordFrame(frame);
            renderDevice.SubmitAndPresent(frame, false);
            graphSmoke.NotifySubmitted(frame.FrameIndex, frame.SubmissionInstance);
        }
        catch (...)
        {
            if (frame.bSubmissionIssued)
            {
                graphSmoke.NotifySubmitted(frame.FrameIndex, frame.SubmissionInstance);
            }
            else
            {
                renderDevice.AbortFrame(frame);
                graphSmoke.AbortFrame(frame.FrameIndex);
            }
            throw;
        }

        if (HEngine::EffekseerRuntime::GetRenderBackendStatus()
            != HEngine::VFXRenderBackendStatus::Ready)
        {
            throw std::runtime_error("Effekseer DX12 interop smoke observed stale frame state");
        }
    }
}

int main()
{
    HEngine::Log::Init();
    HEngine::ConfigManager::GetInstance().Init(
        HE_DEFAULT_PROJECT_DIR,
        HE_ENGINE_CONTENT_DIR,
        {},
        HE_ENGINE_GENERATED_CONTENT_DIR,
        HE_PROJECT_GAME_CONFIG);

    HEngine::Scope<HEngine::Window> window;
    HEngine::RenderDevice renderDevice;
    int result = 1;
    try
    {
        window = HEngine::Window::Create(HEngine::WindowProps(
            "HEngine Effekseer DX12 Interop Smoke",
            c_Width,
            c_Height,
            false,
            true,
            false,
            false,
            false));
        window->SetEventCallback([](HEngine::Event&) {});
        auto* surface = dynamic_cast<HEngine::RenderSurface*>(window.get());
        if (surface == nullptr)
            throw std::runtime_error("Effekseer DX12 interop smoke window has no RenderSurface");

        renderDevice.Initialize(HEngine::RenderBackend::DX12, *surface, true);
        const HEngine::RenderCapabilities& capabilities = renderDevice.GetCapabilities();
        if (!capabilities.bGraphicsQueue || capabilities.FramesInFlight == 0)
            throw std::runtime_error("Effekseer DX12 interop smoke requires a graphics queue and frames in flight");

        const HEngine::EffekseerRuntimeDesc runtimeDesc = {
            .Device = renderDevice.GetDevice(),
            .ColorFormat = nvrhi::Format::RGBA16_FLOAT,
            .DepthFormat = nvrhi::Format::D32,
            .FramesInFlight = capabilities.FramesInFlight,
            .bReversedDepth = false,
        };
        HEngine::EffekseerRuntime::Initialize(runtimeDesc);
        if (!HEngine::EffekseerRuntime::IsInitialized())
            throw std::runtime_error("Effekseer DX12 interop smoke renderer did not initialize");

        if (HEngine::EffekseerRuntime::IsGpuParticleCapable() ||
            HEngine::EffekseerRuntime::IsGpuParticlesEnabled())
        {
            throw std::runtime_error(
                "Effekseer DX12 interop smoke unexpectedly exposed GPU particle support");
        }
        if (HEngine::EffekseerRuntime::PlayEffect(c_GpuEffectPath, glm::vec3(0.0f)) >= 0)
        {
            throw std::runtime_error(
                "Effekseer DX12 interop smoke accepted an unsupported GPU-particle package");
        }
        const HEngine::VFXHandle effectHandle = HEngine::EffekseerRuntime::PlayEffect(
            c_CpuEffectPath,
            glm::vec3(0.0f));
        if (effectHandle < 0 || !HEngine::EffekseerRuntime::IsEffectPlaying(effectHandle))
            throw std::runtime_error("Effekseer DX12 interop smoke failed to start its regression effect");

        auto graphSmoke = std::make_unique<EffekseerGraphSmoke>(
            renderDevice.GetDevice(),
            capabilities.FramesInFlight,
            HEngine::ConfigManager::GetEngineGeneratedContentFullPath("Shaders/CACHES/DXIL"));
        float maxDrawCalls = 0.0f;
        float maxVertexCount = 0.0f;
        float maxCpuInstances = 0.0f;
        for (int frameIndex = 0; frameIndex < c_ActiveDrawFrames; ++frameIndex)
        {
            RunInteropFrame(*window, renderDevice, *graphSmoke);
            const HEngine::VFXStats stats = HEngine::EffekseerRuntime::GetStats();
            maxDrawCalls = std::max(maxDrawCalls, stats.DrawCalls.Current);
            maxVertexCount = std::max(maxVertexCount, stats.VertexCount.Current);
            maxCpuInstances = std::max(maxCpuInstances, stats.ActiveCpuInstances.Current);
        }
        std::cout << "EFFEKSEER_DX12_INTEROP_DRAW_STATS: cpuInstances=" << maxCpuInstances
                  << " drawCalls=" << maxDrawCalls
                  << " vertices=" << maxVertexCount
                  << " gpuCapable=0 gpuEnabled=0\n";
        if (maxDrawCalls < 1.0f || maxVertexCount < 1.0f)
        {
            throw std::runtime_error("Effekseer DX12 interop smoke did not record a real CPU-sprite draw");
        }

        HEngine::EffekseerRuntime::StopEffect(effectHandle);
        if (HEngine::EffekseerRuntime::IsEffectPlaying(effectHandle))
            throw std::runtime_error("Effekseer DX12 interop smoke failed to stop its effect");
        renderDevice.WaitForIdle();
        HEngine::EffekseerRuntime::Shutdown();
        if (HEngine::EffekseerRuntime::IsInitialized())
            throw std::runtime_error("Effekseer DX12 interop smoke did not release its adapter state");

        graphSmoke.reset();
        renderDevice.Shutdown();
        std::cout
            << "EFFEKSEER_DX12_INTEROP_SMOKE_PASS: manager init/update, RenderGraph "
               "D32-to-R32 depth snapshot, HDR background copy, read-only depth VFX pass, "
               "real CPU draw lifecycle, typed D32 PSO/DSV agreement, explicit GPU-particle "
               "capability rejection, existing device/queue, "
               "effect stop, idle release\n";
        result = 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "EFFEKSEER_DX12_INTEROP_SMOKE_FAIL: " << exception.what() << '\n';
        HEngine::EffekseerRuntime::Shutdown();
        if (renderDevice.IsInitialized())
        {
            try
            {
                renderDevice.Shutdown();
            }
            catch (const std::exception& shutdownException)
            {
                std::cerr << "EFFEKSEER_DX12_INTEROP_SMOKE_SHUTDOWN_FAIL: "
                          << shutdownException.what() << '\n';
            }
        }
    }

    window.reset();
    return result;
}
