#include "Runtime/Core/Log/Log.h"
#include "Runtime/Core/Window/Window.h"
#include "Runtime/Graphics/RHI/PipelineCache.h"
#include "Runtime/Graphics/RHI/RenderDevice.h"
#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <HEngine/ProjectBuildConfig.h>

#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
    constexpr int c_InitialWidth = 640;
    constexpr int c_InitialHeight = 360;
    constexpr int c_ResizedWidth = 800;
    constexpr int c_ResizedHeight = 450;
    constexpr uint32_t c_FramesPerSize = 6;

    struct TriangleVertex
    {
        float Position[3];
        float Color[3];
    };

    constexpr std::array<TriangleVertex, 3> c_TriangleVertices = {
        TriangleVertex{ { -0.62f, -0.56f, 0.5f }, { 1.0f, 0.15f, 0.08f } },
        TriangleVertex{ { 0.0f, 0.62f, 0.5f }, { 0.10f, 1.0f, 0.20f } },
        TriangleVertex{ { 0.62f, -0.56f, 0.5f }, { 0.10f, 0.28f, 1.0f } },
    };
    constexpr std::array<uint32_t, 3> c_TriangleIndices = { 0, 1, 2 };

    [[nodiscard]] std::string ReadShaderContentHash(const std::filesystem::path& path)
    {
        std::ifstream stream(path);
        if (!stream)
            throw std::runtime_error("Failed to open shader cache-key sidecar: " + path.string());

        std::string line;
        while (std::getline(stream, line))
        {
            constexpr std::string_view keyPrefix = "key=";
            if (line.starts_with(keyPrefix) && line.size() > keyPrefix.size())
                return line.substr(keyPrefix.size());
        }

        throw std::runtime_error("Shader cache-key sidecar has no content hash: " + path.string());
    }

    class TriangleRenderer final
    {
    public:
        explicit TriangleRenderer(nvrhi::IDevice* device)
            : mDevice(device)
            , mShaderManager(device)
            , mPipelineCache(device)
        {
            if (mDevice == nullptr)
                throw std::invalid_argument("DX12 triangle renderer requires a valid NVRHI device");

            CreateShaders();
            CreateImmutableState();
            CreateGeometry();
        }

        void Record(
            nvrhi::ICommandList* commandList,
            nvrhi::IFramebuffer* framebuffer,
            uint32_t width,
            uint32_t height,
            const nvrhi::Color& clearColor)
        {
            if (commandList == nullptr || framebuffer == nullptr)
                throw std::invalid_argument("DX12 triangle renderer received an invalid command list or framebuffer");

            if (!mGeometryUploaded)
            {
                commandList->writeBuffer(
                    mVertexBuffer,
                    c_TriangleVertices.data(),
                    sizeof(c_TriangleVertices));
                commandList->writeBuffer(
                    mIndexBuffer,
                    c_TriangleIndices.data(),
                    sizeof(c_TriangleIndices));
                mGeometryUploaded = true;
            }

            if (!mPipeline)
            {
                nvrhi::GraphicsPipelineDesc pipelineDesc;
                pipelineDesc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                    .setInputLayout(mInputLayout)
                    .setVertexShader(mVertexShader)
                    .setPixelShader(mPixelShader)
                    .addBindingLayout(mBindingLayout);
                pipelineDesc.renderState.depthStencilState
                    .disableDepthTest()
                    .disableDepthWrite();
                pipelineDesc.renderState.rasterState.setCullNone();

                mPipeline = mPipelineCache.GetOrCreateGraphicsPipeline(
                    HEngine::GraphicsPipelineKey{ 0x4458313254524950ull },
                    pipelineDesc,
                    framebuffer->getFramebufferInfo(),
                    "DX12 smoke indexed triangle");
            }

            commandList->beginMarker("DX12 Smoke Indexed Triangle");
            const nvrhi::FramebufferDesc& framebufferDesc = framebuffer->getDesc();
            if (framebufferDesc.colorAttachments.empty()
                || framebufferDesc.colorAttachments[0].texture == nullptr)
            {
                throw std::runtime_error("DX12 smoke framebuffer has no color attachment");
            }
            commandList->clearTextureFloat(
                framebufferDesc.colorAttachments[0].texture,
                framebufferDesc.colorAttachments[0].subresources,
                clearColor);

            nvrhi::GraphicsState state;
            state.setPipeline(mPipeline)
                .setFramebuffer(framebuffer)
                .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                    nvrhi::Viewport(static_cast<float>(width), static_cast<float>(height))))
                .addBindingSet(mBindingSet)
                .addVertexBuffer(nvrhi::VertexBufferBinding()
                    .setBuffer(mVertexBuffer)
                    .setSlot(0)
                    .setOffset(0))
                .setIndexBuffer(nvrhi::IndexBufferBinding()
                    .setBuffer(mIndexBuffer)
                    .setFormat(nvrhi::Format::R32_UINT)
                    .setOffset(0));
            commandList->setGraphicsState(state);
            commandList->drawIndexed(nvrhi::DrawArguments().setVertexCount(
                static_cast<uint32_t>(c_TriangleIndices.size())));
            commandList->endMarker();
        }

        void ValidateCacheState() const
        {
            if (mShaderManager.Size() != 2
                || mPipelineCache.GetBindingLayoutCount() != 1
                || mPipelineCache.GetStaticBindingSetCount() != 1
                || mPipelineCache.GetGraphicsPipelineCount() != 1)
            {
                throw std::runtime_error("DX12 smoke shader or immutable pipeline cache contains unexpected entries");
            }
        }

    private:
        void CreateShaders()
        {
            const std::filesystem::path shaderCache =
                HEngine::ConfigManager::GetEngineGeneratedContentFullPath("Shaders/CACHES/DXIL");
            const std::filesystem::path vertexPath = shaderCache / "DX12Triangle.vs";
            const std::filesystem::path pixelPath = shaderCache / "DX12Triangle.ps";

            mVertexShader = mShaderManager.LoadDxilFile(
                HEngine::ShaderCacheKey{
                    .Source = "Validation/DX12Triangle.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Vertex,
                    .EntryPoint = "VS",
                    .TargetProfile = "vs_6_0",
                    .ContentHash = ReadShaderContentHash(vertexPath.string() + ".cachekey"),
                },
                vertexPath,
                "DX12 smoke triangle VS");
            mPixelShader = mShaderManager.LoadDxilFile(
                HEngine::ShaderCacheKey{
                    .Source = "Validation/DX12Triangle.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = "ps_6_0",
                    .ContentHash = ReadShaderContentHash(pixelPath.string() + ".cachekey"),
                },
                pixelPath,
                "DX12 smoke triangle PS");
        }

        void CreateImmutableState()
        {
            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc
                .setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
                .setRegisterSpaceAndDescriptorSet(0);
            mBindingLayout = mPipelineCache.GetOrCreateBindingLayout(
                HEngine::BindingLayoutKey{ 0x44583132454d5054ull },
                layoutDesc,
                "DX12 smoke empty space0 layout");
            mBindingSet = mPipelineCache.GetOrCreateStaticBindingSet(
                HEngine::StaticBindingSetKey{ 0x44583132454d5053ull },
                nvrhi::BindingSetDesc{},
                mBindingLayout,
                "DX12 smoke empty space0 binding set");

            const std::array<nvrhi::VertexAttributeDesc, 2> attributes = {
                nvrhi::VertexAttributeDesc()
                    .setName("POSITION")
                    .setFormat(nvrhi::Format::RGB32_FLOAT)
                    .setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(TriangleVertex, Position)))
                    .setElementStride(sizeof(TriangleVertex)),
                nvrhi::VertexAttributeDesc()
                    .setName("COLOR")
                    .setFormat(nvrhi::Format::RGB32_FLOAT)
                    .setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(TriangleVertex, Color)))
                    .setElementStride(sizeof(TriangleVertex)),
            };
            mInputLayout = mDevice->createInputLayout(
                attributes.data(),
                static_cast<uint32_t>(attributes.size()),
                mVertexShader);
            if (!mInputLayout)
                throw std::runtime_error("NVRHI failed to create the DX12 smoke triangle input layout");
        }

        void CreateGeometry()
        {
            nvrhi::BufferDesc vertexDesc;
            vertexDesc.setByteSize(sizeof(c_TriangleVertices))
                .setIsVertexBuffer(true)
                .enableAutomaticStateTracking(nvrhi::ResourceStates::VertexBuffer)
                .setDebugName("DX12 Smoke Triangle Vertices");
            mVertexBuffer = mDevice->createBuffer(vertexDesc);

            nvrhi::BufferDesc indexDesc;
            indexDesc.setByteSize(sizeof(c_TriangleIndices))
                .setIsIndexBuffer(true)
                .enableAutomaticStateTracking(nvrhi::ResourceStates::IndexBuffer)
                .setDebugName("DX12 Smoke Triangle Indices");
            mIndexBuffer = mDevice->createBuffer(indexDesc);
            if (!mVertexBuffer || !mIndexBuffer)
                throw std::runtime_error("NVRHI failed to create DX12 smoke triangle geometry buffers");
        }

        nvrhi::DeviceHandle mDevice;
        HEngine::ShaderManager mShaderManager;
        HEngine::PipelineCache mPipelineCache;
        nvrhi::ShaderHandle mVertexShader;
        nvrhi::ShaderHandle mPixelShader;
        nvrhi::BindingLayoutHandle mBindingLayout;
        nvrhi::BindingSetHandle mBindingSet;
        nvrhi::InputLayoutHandle mInputLayout;
        nvrhi::GraphicsPipelineHandle mPipeline;
        nvrhi::BufferHandle mVertexBuffer;
        nvrhi::BufferHandle mIndexBuffer;
        bool mGeometryUploaded = false;
    };

    void RenderFrames(
        HEngine::Window& window,
        HEngine::RenderDevice& renderDevice,
        TriangleRenderer& triangleRenderer,
        uint32_t frameCount,
        uint32_t colorOffset)
    {
        const std::array<nvrhi::Color, 4> clearColors = {
            nvrhi::Color(0.04f, 0.08f, 0.16f, 1.f),
            nvrhi::Color(0.12f, 0.04f, 0.18f, 1.f),
            nvrhi::Color(0.02f, 0.16f, 0.12f, 1.f),
            nvrhi::Color(0.18f, 0.10f, 0.02f, 1.f),
        };

        for (uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            std::cout << "DX12_SMOKE_STAGE: begin frame " << frameIndex << std::endl;
            window.OnUpdate();
            HEngine::FrameContext& frame = renderDevice.BeginFrame();
            HEngine::SwapChain& swapChain = renderDevice.GetSwapChain();
            if (swapChain.GetCurrentBackBuffer() == nullptr || swapChain.GetCurrentFramebuffer() == nullptr)
                throw std::runtime_error("DX12 smoke test acquired an invalid imported backbuffer or framebuffer");

            triangleRenderer.Record(
                frame.GraphicsCommandList,
                swapChain.GetCurrentFramebuffer(),
                swapChain.GetWidth(),
                swapChain.GetHeight(),
                clearColors[(colorOffset + frameIndex) % clearColors.size()]);
            std::cout << "DX12_SMOKE_STAGE: submit frame " << frameIndex << std::endl;
            renderDevice.SubmitAndPresent(frame, false);
            std::cout << "DX12_SMOKE_STAGE: presented frame " << frameIndex << std::endl;
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
            "HEngine DX12 Smoke",
            c_InitialWidth,
            c_InitialHeight,
            false,
            true,
            false,
            false,
            false));
        window->SetEventCallback([](HEngine::Event&) {});
        auto* renderSurface = dynamic_cast<HEngine::RenderSurface*>(window.get());
        if (renderSurface == nullptr)
            throw std::runtime_error("HEngine window does not provide a RenderSurface");

        renderDevice.Initialize(HEngine::RenderBackend::DX12, *renderSurface, true);
        const HEngine::RenderCapabilities& capabilities = renderDevice.GetCapabilities();
        if (!capabilities.bGraphicsQueue || !capabilities.bComputeQueue || !capabilities.bCopyQueue
            || !capabilities.bValidationLayer || capabilities.FramesInFlight != 3)
        {
            throw std::runtime_error("DX12 smoke test observed incomplete render-device capabilities");
        }

        std::unique_ptr<TriangleRenderer> triangleRenderer =
            std::make_unique<TriangleRenderer>(renderDevice.GetDevice());

        std::cout << "DX12_SMOKE_STAGE: abort unsubmitted frame" << std::endl;
        HEngine::FrameContext& abortedFrame = renderDevice.BeginFrame();
        renderDevice.AbortFrame(abortedFrame);
        if (abortedFrame.bSubmissionIssued)
            throw std::runtime_error("DX12 smoke abort incorrectly marked a frame submitted");

        std::cout << "DX12_SMOKE_STAGE: initial frames" << std::endl;
        RenderFrames(*window, renderDevice, *triangleRenderer, c_FramesPerSize, 0);

        std::cout << "DX12_SMOKE_STAGE: resize" << std::endl;
        std::cout << "DX12_SMOKE_STAGE: resize backend" << std::endl;
        renderDevice.Resize(c_ResizedWidth, c_ResizedHeight);
        std::cout << "DX12_SMOKE_STAGE: resize backend complete" << std::endl;
        if (renderDevice.GetSwapChain().GetWidth() != c_ResizedWidth
            || renderDevice.GetSwapChain().GetHeight() != c_ResizedHeight)
        {
            throw std::runtime_error("DX12 smoke test swap-chain resize did not take effect");
        }

        std::cout << "DX12_SMOKE_STAGE: resized frames" << std::endl;
        RenderFrames(
            *window,
            renderDevice,
            *triangleRenderer,
            c_FramesPerSize,
            c_FramesPerSize);
        triangleRenderer->ValidateCacheState();
        std::cout << "DX12_SMOKE_STAGE: wait idle" << std::endl;
        renderDevice.WaitForIdle();
        triangleRenderer.reset();
        std::cout << "DX12_SMOKE_STAGE: shutdown" << std::endl;
        renderDevice.Shutdown();

        std::cout << "DX12_SMOKE_PASS: abort-frame recovery, DXC DXIL, immutable pipeline cache, indexed triangle, validation, present, resize, GC, and shutdown\n";
        result = 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "DX12_SMOKE_FAIL: " << exception.what() << '\n';
        if (renderDevice.IsInitialized())
        {
            try
            {
                renderDevice.Shutdown();
            }
            catch (const std::exception& shutdownException)
            {
                std::cerr << "DX12_SMOKE_SHUTDOWN_FAIL: " << shutdownException.what() << '\n';
            }
        }
    }

    window.reset();
    return result;
}
