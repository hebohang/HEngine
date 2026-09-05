#include "Runtime/Core/Log/Log.h"
#include "Runtime/Core/Window/Window.h"
#include "Runtime/Graphics/RHI/RenderDevice.h"
#include "Runtime/ImGui/NvrhiImGuiLayer.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <HEngine/ProjectBuildConfig.h>

#include <imgui.h>

#include <array>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace
{
    constexpr uint32_t c_InitialWidth = 720;
    constexpr uint32_t c_InitialHeight = 420;
    constexpr uint32_t c_ResizedWidth = 896;
    constexpr uint32_t c_ResizedHeight = 512;
    constexpr uint32_t c_FrameCount = 10;

    constexpr std::array<uint32_t, 16> c_CheckerPixels = {
        0xffe5a23bu, 0xff365bd8u, 0xffe5a23bu, 0xff365bd8u,
        0xff365bd8u, 0xffe5a23bu, 0xff365bd8u, 0xffe5a23bu,
        0xffe5a23bu, 0xff365bd8u, 0xffe5a23bu, 0xff365bd8u,
        0xff365bd8u, 0xffe5a23bu, 0xff365bd8u, 0xffe5a23bu,
    };

    [[nodiscard]] nvrhi::TextureHandle CreateCheckerTexture(nvrhi::IDevice* device)
    {
        nvrhi::TextureDesc desc;
        desc.setWidth(4)
            .setHeight(4)
            .setFormat(nvrhi::Format::RGBA8_UNORM)
            .setDimension(nvrhi::TextureDimension::Texture2D)
            .setDebugName("NVRHI ImGui Smoke Checker")
            .enableAutomaticStateTracking(nvrhi::ResourceStates::ShaderResource);
        nvrhi::TextureHandle texture = device->createTexture(desc);
        if (!texture)
            throw std::runtime_error("Failed to create the ImGui smoke checker texture");
        return texture;
    }

    void BuildUi(HEngine::ImGuiTextureHandle checkerTexture, uint32_t frameNumber)
    {
        ImGui::SetNextWindowPos(ImVec2(24.f, 24.f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(360.f, 250.f), ImGuiCond_Always);
        ImGui::Begin("NVRHI ImGui DX12 Smoke", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
        ImGui::Text("Frame %u", frameNumber);
        ImGui::TextUnformatted("DXC DXIL + NVRHI + typed texture handles");
        ImGui::Separator();
        ImGui::Image(
            HEngine::NvrhiImGuiLayer::ToImTextureID(checkerTexture),
            ImVec2(128.f, 128.f));
        ImGui::End();
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
    std::unique_ptr<HEngine::ImGuiLayer> layerOwner;
    HEngine::NvrhiImGuiLayer* layer = nullptr;
    nvrhi::TextureHandle checkerTexture;
    int result = 1;

    try
    {
        window = HEngine::Window::Create(HEngine::WindowProps(
            "HEngine NVRHI ImGui DX12 Smoke",
            c_InitialWidth,
            c_InitialHeight,
            false,
            true,
            false,
            false,
            false));
        window->SetEventCallback([](HEngine::Event&) {});
        auto* renderSurface = dynamic_cast<HEngine::RenderSurface*>(window.get());
        if (!renderSurface)
            throw std::runtime_error("HEngine window does not expose a RenderSurface");

        renderDevice.Initialize(HEngine::RenderBackend::DX12, *renderSurface, true);
        const std::filesystem::path shaderCache =
            HEngine::ConfigManager::GetEngineGeneratedContentFullPath("Shaders/CACHES/DXIL");
        layerOwner.reset(HEngine::ImGuiLayer::CreateNvrhi(
            HEngine::NvrhiImGuiLayerDesc{
                .MainWindow = window.get(),
                .Renderer = HEngine::NvrhiImGuiRendererDesc{
                    .Device = renderDevice.GetDevice(),
                    .FramesInFlight = renderDevice.GetCapabilities().FramesInFlight,
                    .VertexShaderDxil = shaderCache / "NvrhiImGui.vs",
                    .PixelShaderDxil = shaderCache / "NvrhiImGui.ps",
                    .DebugName = "NVRHI ImGui DX12 Smoke",
                },
                .bInstallGlfwCallbacks = true,
                .bEnableDocking = true,
            }));
        layer = dynamic_cast<HEngine::NvrhiImGuiLayer*>(layerOwner.get());
        if (!layer || !layer->IsInitialized() || ImGui::GetCurrentContext() == nullptr)
            throw std::runtime_error("NVRHI ImGui layer factory initialization failed");
        layer->OnAttach();
        if (layer->AreSecondaryViewportsEnabled()
            || (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
        {
            throw std::runtime_error("NVRHI ImGui layer enabled unsupported secondary viewports");
        }

        checkerTexture = CreateCheckerTexture(renderDevice.GetDevice());
        const HEngine::ImGuiTextureHandle checkerHandle = layer->RegisterTexture(checkerTexture);
        if (!layer->IsTextureValid(checkerHandle)
            || HEngine::NvrhiImGuiLayer::FromImTextureID(
                HEngine::NvrhiImGuiLayer::ToImTextureID(checkerHandle)) != checkerHandle)
        {
            throw std::runtime_error("Typed ImGui texture handle round trip failed");
        }

        HEngine::ImGuiTextureHandle retiredHandle;
        HEngine::ImGuiTextureHandle recycledHandle;
        uint32_t retiredFrameSlot = 0;
        bool bCheckerUploaded = false;
        bool bRetireScheduled = false;
        bool bIndexRecycled = false;

        for (uint32_t frameNumber = 0; frameNumber < c_FrameCount; ++frameNumber)
        {
            if (frameNumber == 5)
            {
                renderDevice.Resize(c_ResizedWidth, c_ResizedHeight);
                if (renderDevice.GetSwapChain().GetWidth() != c_ResizedWidth
                    || renderDevice.GetSwapChain().GetHeight() != c_ResizedHeight)
                {
                    throw std::runtime_error("ImGui smoke swap-chain resize failed");
                }
            }

            window->OnUpdate();
            HEngine::SwapChain& swapChain = renderDevice.GetSwapChain();
            if (frameNumber == 2)
                ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
            layer->BeginCpuFrame();
            if (layer->AreSecondaryViewportsEnabled())
                throw std::runtime_error("NVRHI ImGui layer did not reject secondary viewports");

            BuildUi(checkerHandle, frameNumber);
            HEngine::FrameContext& frame = renderDevice.BeginFrame();
            try
            {
                if (!bCheckerUploaded)
                {
                    frame.GraphicsCommandList->writeTexture(
                        checkerTexture,
                        0,
                        0,
                        c_CheckerPixels.data(),
                        4u * sizeof(uint32_t));
                    bCheckerUploaded = true;
                }

                if (frameNumber == 1)
                {
                    retiredHandle = layer->RegisterTexture(checkerTexture);
                    retiredFrameSlot = frame.FrameIndex;
                    layer->UnregisterTexture(retiredHandle, retiredFrameSlot);
                    if (layer->IsTextureValid(retiredHandle))
                        throw std::runtime_error("Retired ImGui texture handle remained valid");
                    bRetireScheduled = true;
                }
                else if (bRetireScheduled && !bIndexRecycled
                    && frameNumber > 1 && frame.FrameIndex == retiredFrameSlot)
                {
                    recycledHandle = layer->RegisterTexture(checkerTexture);
                    if (recycledHandle.Index != retiredHandle.Index
                        || recycledHandle.Generation == retiredHandle.Generation
                        || layer->IsTextureValid(retiredHandle))
                    {
                        throw std::runtime_error("Deferred ImGui texture index generation contract failed");
                    }
                    bIndexRecycled = true;
                }

                const nvrhi::FramebufferDesc& framebufferDesc =
                    swapChain.GetCurrentFramebuffer()->getDesc();
                frame.GraphicsCommandList->clearTextureFloat(
                    framebufferDesc.colorAttachments[0].texture,
                    framebufferDesc.colorAttachments[0].subresources,
                    nvrhi::Color(0.035f, 0.045f, 0.07f, 1.f));
                layer->RecordGpuFrame(
                    frame.GraphicsCommandList,
                    swapChain.GetCurrentFramebuffer(),
                    frame.FrameIndex);
                renderDevice.SubmitAndPresent(frame, false);
            }
            catch (...)
            {
                layer->CancelCpuFrame();
                if (!frame.bSubmissionIssued)
                    renderDevice.AbortFrame(frame);
                throw;
            }
        }

        if (!bIndexRecycled)
            throw std::runtime_error("ImGui smoke did not reacquire the retired frame slot");
        if (!layer->GetFontTexture() || layer->GetRegisteredTextureCount() < 3)
            throw std::runtime_error("ImGui font/custom texture registration is incomplete");
        const HEngine::NvrhiImGuiRendererStats& stats = layer->GetRendererStats();
        if (stats.DrawCalls == 0 || stats.RenderedVertices == 0 || stats.RenderedIndices == 0
            || stats.SkippedInvalidTextures != 0)
        {
            throw std::runtime_error("ImGui smoke observed invalid renderer statistics");
        }

        renderDevice.WaitForIdle();
        layer->OnDetach();
        layer = nullptr;
        layerOwner.reset();
        checkerTexture = nullptr;
        renderDevice.Shutdown();
        window.reset();

        std::cout
            << "IMGUI_DX12_SMOKE_PASS: GLFW platform/input + NVRHI layer recording, dynamic font "
            << "upload, typed textures, deferred index recycling, disabled secondary viewports, "
            << "resize, validation, and shutdown\n";
        result = 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "IMGUI_DX12_SMOKE_FAIL: " << exception.what() << '\n';
        if (renderDevice.IsInitialized())
        {
            try
            {
                renderDevice.WaitForIdle();
            }
            catch (...)
            {
            }
        }
        if (layer)
        {
            layer->CancelCpuFrame();
            try { layer->OnDetach(); } catch (...) {}
        }
        layer = nullptr;
        layerOwner.reset();
        checkerTexture = nullptr;
        if (renderDevice.IsInitialized())
        {
            try
            {
                renderDevice.Shutdown();
            }
            catch (...)
            {
            }
        }
        window.reset();
    }

    return result;
}
