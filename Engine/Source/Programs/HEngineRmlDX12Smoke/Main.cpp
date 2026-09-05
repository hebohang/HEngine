#include "Runtime/Core/Log/Log.h"
#include "Runtime/Core/PresentationClock.h"
#include "Runtime/Core/Window/Window.h"
#include "Runtime/Graphics/RHI/RenderDevice.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <HEngine/ProjectBuildConfig.h>
#include "Runtime/UI/Rml/RmlUiManager.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>

#include <array>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t c_InitialWidth = 720;
    constexpr uint32_t c_InitialHeight = 420;
    constexpr uint32_t c_ResizedWidth = 912;
    constexpr uint32_t c_ResizedHeight = 528;
    constexpr uint32_t c_FrameCount = 12;
    constexpr const char* c_Document = R"(
<rml>
<head>
<style>
body { width: 100%; height: 100%; margin: 0px; }
@keyframes smoke-pulse { from { opacity: 0.25; } to { opacity: 0.9; } }
#panel {
    position: absolute; left: 28px; top: 24px;
    width: 340px; height: 190px;
    background-color: #294a78e8;
    border-radius: 34px;
    overflow: hidden;
}
#clip {
    position: absolute; left: 24px; top: 26px;
    width: 170px; height: 92px;
    background-color: #18304fff;
    border-radius: 20px;
    overflow: hidden;
}
#wide {
    position: absolute; left: 36px; top: 22px;
    width: 260px; height: 58px;
    background-color: #e58f38ff;
}
#event-target {
    position: absolute; left: 386px; top: 220px;
    width: 180px; height: 70px;
    background-color: #8ce0c5ff;
    opacity: 1;
    transition: opacity 0.05s;
}
#event-target.faded { opacity: 0.2; }
#filtered-panel {
    position: absolute; left: 392px; top: 28px;
    width: 152px; height: 76px;
    background-color: #d45f9ce8;
    filter: opacity(0.82) blur(2px) saturate(1.1);
}
#filtered-panel-child {
    position: absolute; left: 28px; top: 16px;
    width: 96px; height: 42px;
    background-color: #55d8f0d8;
}
#smoke-lottie { position: absolute; left: 585px; top: 210px; width: 96px; height: 96px; }
#smoke-svg { position: absolute; left: 390px; top: 305px; width: 96px; height: 96px; }
</style>
</head>
<body>
    <div id="panel"><div id="clip"><div id="wide"></div></div></div>
    <div id="filtered-panel"><div id="filtered-panel-child"></div></div>
    <div id="event-target"></div>
    <lottie id="smoke-lottie" src="showcase-burst.lottie.json"></lottie>
    <svg id="smoke-svg" src="showcase-starburst.svg"></svg>
</body>
</rml>
)";

    struct DirectResources
    {
        Rml::CompiledGeometryHandle Geometry = 0;
        Rml::TextureHandle Texture = 0;
    };

    struct SmokeRenderTarget
    {
        nvrhi::TextureHandle Color;
        nvrhi::TextureHandle DepthStencil;
        nvrhi::FramebufferHandle Framebuffer;
    };

    [[nodiscard]] SmokeRenderTarget CreateSmokeRenderTarget(
        nvrhi::IDevice* device,
        uint32_t width,
        uint32_t height,
        bool bInverseStencilClear)
    {
        const char* targetKind = bInverseStencilClear ? "Inverse" : "Regular";
        nvrhi::TextureDesc colorDesc;
        colorDesc.setWidth(width)
            .setHeight(height)
            .setFormat(nvrhi::Format::RGBA8_UNORM)
            .setDimension(nvrhi::TextureDimension::Texture2D)
            .setIsRenderTarget(true)
            .setClearValue(nvrhi::Color(0.f, 0.f, 0.f, 0.f))
            .setDebugName(std::string("RmlUi DX12 Smoke RGBA8 ") + targetKind + " Target")
            .enableAutomaticStateTracking(nvrhi::ResourceStates::RenderTarget);

        nvrhi::TextureDesc depthStencilDesc;
        depthStencilDesc.setWidth(width)
            .setHeight(height)
            .setFormat(nvrhi::Format::D24S8)
            .setDimension(nvrhi::TextureDimension::Texture2D)
            .setIsRenderTarget(true)
            .setClearValue(nvrhi::Color(
                1.f,
                bInverseStencilClear ? 1.f : 0.f,
                0.f,
                0.f))
            .setDebugName(std::string("RmlUi DX12 Smoke D24S8 ") + targetKind + " Target")
            .enableAutomaticStateTracking(nvrhi::ResourceStates::DepthWrite);
        depthStencilDesc.isShaderResource = false;

        SmokeRenderTarget target;
        target.Color = device->createTexture(colorDesc);
        target.DepthStencil = device->createTexture(depthStencilDesc);
        if (!target.Color || !target.DepthStencil)
            throw std::runtime_error("Failed to create the RmlUi smoke RGBA8+D24S8 targets");

        nvrhi::FramebufferDesc framebufferDesc;
        framebufferDesc.addColorAttachment(target.Color)
            .setDepthAttachment(target.DepthStencil);
        target.Framebuffer = device->createFramebuffer(framebufferDesc);
        if (!target.Framebuffer)
            throw std::runtime_error("Failed to create the RmlUi smoke RGBA8+D24S8 framebuffer");
        return target;
    }

    class CompletionListener final : public Rml::EventListener
    {
    public:
        void ProcessEvent(Rml::Event& event) override
        {
            Rml::Element* target = event.GetTargetElement();
            if (!target || target->GetId() != "event-target")
                throw std::runtime_error("RmlUi completion event did not preserve its exact target");
            if (event.GetCurrentElement() != event.GetTargetElement()->GetOwnerDocument())
                throw std::runtime_error("RmlUi completion event did not bubble through the document root");
            if (event.GetParameter<Rml::String>("property", "").empty())
                throw std::runtime_error("RmlUi completion event omitted its property payload");
            if (event.GetType() == "animationend")
                ++AnimationEndCount;
            else if (event.GetType() == "transitionend")
                ++TransitionEndCount;
        }

        uint32_t AnimationEndCount = 0;
        uint32_t TransitionEndCount = 0;
    };

    [[nodiscard]] DirectResources CreateDirectResources(Rml::RenderInterface& renderInterface)
    {
        const Rml::ColourbPremultiplied white(255, 255, 255, 255);
        const std::array<Rml::Vertex, 4> vertices = {
            Rml::Vertex{ Rml::Vector2f(0.f, 0.f), white, Rml::Vector2f(0.f, 0.f) },
            Rml::Vertex{ Rml::Vector2f(220.f, 0.f), white, Rml::Vector2f(1.f, 0.f) },
            Rml::Vertex{ Rml::Vector2f(220.f, 132.f), white, Rml::Vector2f(1.f, 1.f) },
            Rml::Vertex{ Rml::Vector2f(0.f, 132.f), white, Rml::Vector2f(0.f, 1.f) },
        };
        constexpr std::array<int, 6> indices = { 0, 1, 2, 0, 2, 3 };

        std::vector<Rml::byte> pixels(4u * 4u * 4u);
        for (uint32_t y = 0; y < 4; ++y)
        {
            for (uint32_t x = 0; x < 4; ++x)
            {
                const bool bOrange = ((x + y) & 1u) == 0u;
                const size_t offset = static_cast<size_t>(y * 4u + x) * 4u;
                pixels[offset + 0] = bOrange ? 238 : 48;
                pixels[offset + 1] = bOrange ? 154 : 98;
                pixels[offset + 2] = bOrange ? 48 : 220;
                pixels[offset + 3] = 255;
            }
        }

        DirectResources resources;
        resources.Geometry = renderInterface.CompileGeometry(
            Rml::Span<const Rml::Vertex>(vertices.data(), vertices.size()),
            Rml::Span<const int>(indices.data(), indices.size()));
        resources.Texture = renderInterface.GenerateTexture(
            Rml::Span<const Rml::byte>(pixels.data(), pixels.size()),
            Rml::Vector2i(4, 4));
        if (resources.Geometry == 0 || resources.Texture == 0)
            throw std::runtime_error("Failed to create direct RmlUi smoke geometry or generated texture");
        return resources;
    }

    void RenderDirectResources(
        Rml::RenderInterface& renderInterface,
        const DirectResources& resources)
    {
        renderInterface.SetTransform(nullptr);
        renderInterface.EnableScissorRegion(true);
        renderInterface.SetScissorRegion(Rml::Rectanglei::FromPositionSize(
            Rml::Vector2i(414, 64), Rml::Vector2i(174, 106)));
        renderInterface.RenderGeometry(
            resources.Geometry,
            Rml::Vector2f(404.f, 58.f),
            resources.Texture);
        renderInterface.EnableScissorRegion(false);
    }

    void ExerciseInverseClipMask(
        Rml::RenderInterface& renderInterface,
        const DirectResources& resources)
    {
        renderInterface.SetTransform(nullptr);
        renderInterface.EnableScissorRegion(false);
        renderInterface.EnableClipMask(true);
        renderInterface.RenderToClipMask(
            Rml::ClipMaskOperation::SetInverse,
            resources.Geometry,
            Rml::Vector2f(492.f, 112.f));
        renderInterface.RenderGeometry(
            resources.Geometry,
            Rml::Vector2f(470.f, 92.f),
            resources.Texture);
        renderInterface.EnableClipMask(false);
    }

    void ExerciseAdvancedRenderInterface(
        Rml::RenderInterface& renderInterface,
        const DirectResources& resources)
    {
        const Rml::LayerHandle contentLayer = renderInterface.PushLayer();
        if (!contentLayer)
            throw std::runtime_error("RmlUi smoke could not allocate a content layer");
        RenderDirectResources(renderInterface, resources);

        const Rml::LayerHandle maskLayer = renderInterface.PushLayer();
        if (!maskLayer)
            throw std::runtime_error("RmlUi smoke could not allocate a mask layer");
        RenderDirectResources(renderInterface, resources);
        const Rml::CompiledFilterHandle maskFilter = renderInterface.SaveLayerAsMaskImage();
        if (!maskFilter)
            throw std::runtime_error("RmlUi smoke could not save a layer as a mask image");

        renderInterface.PopLayer();
        const std::array<Rml::CompiledFilterHandle, 1> filters = { maskFilter };
        renderInterface.CompositeLayers(
            contentLayer,
            {},
            Rml::BlendMode::Blend,
            Rml::Span<const Rml::CompiledFilterHandle>(filters.data(), filters.size()));
        renderInterface.PopLayer();
        renderInterface.ReleaseFilter(maskFilter);

        const Rml::CompiledShaderHandle shader = renderInterface.CompileShader(
            "shader",
            Rml::Dictionary{
                { "value", Rml::Variant(Rml::String("he-sheen")) },
                { "dimensions", Rml::Variant(Rml::Vector2f(220.f, 132.f)) },
            });
        if (!shader)
            throw std::runtime_error("RmlUi smoke could not compile a whitelisted custom shader");
        renderInterface.RenderShader(
            shader,
            resources.Geometry,
            Rml::Vector2f(404.f, 58.f),
            resources.Texture);
        renderInterface.ReleaseShader(shader);
    }

    void ReleaseDirectResources(
        Rml::RenderInterface& renderInterface,
        DirectResources& resources)
    {
        if (resources.Geometry != 0)
            renderInterface.ReleaseGeometry(resources.Geometry);
        if (resources.Texture != 0)
            renderInterface.ReleaseTexture(resources.Texture);
        resources = {};
    }
}

int main()
{
    HEngine::Log::Init();
    HEngine::PresentationClock::GetInstance().Reset();
    HEngine::ConfigManager::GetInstance().Init(
        HE_DEFAULT_PROJECT_DIR,
        HE_ENGINE_CONTENT_DIR,
        {},
        HE_ENGINE_GENERATED_CONTENT_DIR,
        HE_PROJECT_GAME_CONFIG);

    HEngine::Scope<HEngine::Window> window;
    HEngine::RenderDevice renderDevice;
    HEngine::RmlUiManager& rmlManager = HEngine::RmlUiManager::GetInstance();
    Rml::Context* context = nullptr;
    Rml::ElementDocument* document = nullptr;
    DirectResources directResources;
    SmokeRenderTarget smokeRenderTarget;
    SmokeRenderTarget inverseSmokeRenderTarget;
    CompletionListener completionListener;
    int result = 1;

    try
    {
        window = HEngine::Window::Create(HEngine::WindowProps(
            "HEngine NVRHI RmlUi DX12 Smoke",
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
        smokeRenderTarget = CreateSmokeRenderTarget(
            renderDevice.GetDevice(),
            c_InitialWidth,
            c_InitialHeight,
            false);
        inverseSmokeRenderTarget = CreateSmokeRenderTarget(
            renderDevice.GetDevice(),
            c_InitialWidth,
            c_InitialHeight,
            true);
        const std::filesystem::path shaderCache =
            HEngine::ConfigManager::GetEngineGeneratedContentFullPath("Shaders/CACHES/DXIL");
        rmlManager.InitNvrhi(
            HEngine::NvrhiRmlRenderBackendDesc{
                .Device = renderDevice.GetDevice(),
                .FramesInFlight = renderDevice.GetCapabilities().FramesInFlight,
                .VertexShaderArtifact = shaderCache / "NvrhiRml.vs",
                .PixelShaderArtifact = shaderCache / "NvrhiRml.ps",
                .DebugName = "NVRHI RmlUi DX12 Smoke",
            },
            *window);
        context = Rml::GetContext(0);
        if (!context)
            throw std::runtime_error("RmlUi manager did not create its runtime context");
        const std::filesystem::path motionDirectory =
            HEngine::ConfigManager::GetFullPath("Assets/UI/Motion");
        std::string documentUrl = (motionDirectory / "nvrhi-rml-smoke.rml").generic_string();
        if (documentUrl.size() >= 3 && documentUrl[1] == ':' && documentUrl[2] == '/')
            documentUrl.insert(2, "/");
        document = context->LoadDocumentFromMemory(
            c_Document,
            documentUrl);
        if (!document)
            throw std::runtime_error("RmlUi smoke document creation failed");
        document->Show();
        document->AddEventListener("animationend", &completionListener, false);
        document->AddEventListener("transitionend", &completionListener, false);

        Rml::RenderInterface* renderInterfacePtr = Rml::GetRenderInterface();
        if (!renderInterfacePtr)
            throw std::runtime_error("RmlUi manager did not install its NVRHI render interface");
        Rml::RenderInterface& renderInterface = *renderInterfacePtr;
        directResources = CreateDirectResources(renderInterface);
        Rml::CompiledGeometryHandle retiredGeometry = 0;
        Rml::TextureHandle retiredTexture = 0;
        bool bReleased = false;
        bool bRecycled = false;
        uint64_t abortedGeometryUploads = 0;
        uint64_t abortedTextureUploads = 0;

        // Record first-use uploads and intentionally discard the command list.
        // The next submitted frame must upload these resources again.
        rmlManager.BeginFrame(
            static_cast<float>(c_InitialWidth),
            static_cast<float>(c_InitialHeight),
            1.f);
        HEngine::FrameContext& abortedFrame = renderDevice.BeginFrame();
        try
        {
            abortedFrame.GraphicsCommandList->clearTextureFloat(
                smokeRenderTarget.Color,
                nvrhi::AllSubresources,
                nvrhi::Color(0.f, 0.f, 0.f, 0.f));
            abortedFrame.GraphicsCommandList->clearDepthStencilTexture(
                smokeRenderTarget.DepthStencil,
                nvrhi::AllSubresources,
                true,
                1.f,
                true,
                0);
            rmlManager.BeginGpuFrame(
                abortedFrame.GraphicsCommandList,
                smokeRenderTarget.Framebuffer,
                abortedFrame.FrameIndex,
                abortedFrame.CompletedSubmissionInstance);
            RenderDirectResources(renderInterface, directResources);
            rmlManager.EndGpuFrame();

            const HEngine::NvrhiRmlRenderBackendStats& abortedStats =
                rmlManager.GetRenderStats();
            abortedGeometryUploads = abortedStats.GeometryUploads;
            abortedTextureUploads = abortedStats.TextureUploads;
            if (abortedGeometryUploads == 0 || abortedTextureUploads == 0)
            {
                throw std::runtime_error(
                    "RmlUi smoke aborted frame did not record first-use uploads");
            }

            rmlManager.AbortGpuFrame(abortedFrame.FrameIndex);
            renderDevice.AbortFrame(abortedFrame);
        }
        catch (...)
        {
            if (rmlManager.IsGpuFrameActive())
            {
                try
                {
                    rmlManager.EndGpuFrame();
                }
                catch (...)
                {
                }
            }
            rmlManager.AbortGpuFrame(abortedFrame.FrameIndex);
            if (!abortedFrame.bSubmissionIssued)
                renderDevice.AbortFrame(abortedFrame);
            throw;
        }

        for (uint32_t frameNumber = 0; frameNumber < c_FrameCount; ++frameNumber)
        {
            (void)HEngine::PresentationClock::GetInstance().BeginFrame(1.0f / 60.0f);
            if (frameNumber == 1)
            {
                Rml::Element* eventTarget = document->GetElementById("event-target");
                if (!eventTarget)
                    throw std::runtime_error("RmlUi smoke event target is missing");
                eventTarget->SetClass("faded", true);
                if (!eventTarget->Animate("width", Rml::Property(210.0f, Rml::Unit::PX), 0.05f))
                {
                    throw std::runtime_error("RmlUi smoke could not start its animation and transition");
                }
            }
            if (frameNumber == 6)
            {
                renderDevice.Resize(c_ResizedWidth, c_ResizedHeight);
                if (renderDevice.GetSwapChain().GetWidth() != c_ResizedWidth
                    || renderDevice.GetSwapChain().GetHeight() != c_ResizedHeight)
                {
                    throw std::runtime_error("RmlUi smoke swap-chain resize failed");
                }
                smokeRenderTarget = CreateSmokeRenderTarget(
                    renderDevice.GetDevice(),
                    c_ResizedWidth,
                    c_ResizedHeight,
                    false);
                inverseSmokeRenderTarget = CreateSmokeRenderTarget(
                    renderDevice.GetDevice(),
                    c_ResizedWidth,
                    c_ResizedHeight,
                    true);
            }

            window->OnUpdate();
            HEngine::SwapChain& swapChain = renderDevice.GetSwapChain();
            rmlManager.BeginFrame(
                static_cast<float>(swapChain.GetWidth()),
                static_cast<float>(swapChain.GetHeight()),
                1.f);
            rmlManager.Update();

            HEngine::FrameContext& frame = renderDevice.BeginFrame();
            try
            {
                const bool bInverseClipFrame = frameNumber + 1u == c_FrameCount;
                SmokeRenderTarget& activeRenderTarget = bInverseClipFrame
                    ? inverseSmokeRenderTarget
                    : smokeRenderTarget;
                nvrhi::IFramebuffer* swapChainFramebuffer = swapChain.GetCurrentFramebuffer();
                const nvrhi::FramebufferDesc& swapChainFramebufferDesc =
                    swapChainFramebuffer->getDesc();
                frame.GraphicsCommandList->clearTextureFloat(
                    swapChainFramebufferDesc.colorAttachments[0].texture,
                    swapChainFramebufferDesc.colorAttachments[0].subresources,
                    nvrhi::Color(0.025f, 0.035f, 0.065f, 1.f));
                const nvrhi::FramebufferDesc& framebufferDesc =
                    activeRenderTarget.Framebuffer->getDesc();
                if (framebufferDesc.colorAttachments.empty()
                    || framebufferDesc.colorAttachments[0].texture->getDesc().format
                        != nvrhi::Format::RGBA8_UNORM
                    || !framebufferDesc.depthAttachment.valid()
                    || framebufferDesc.depthAttachment.texture->getDesc().format
                        != nvrhi::Format::D24S8)
                {
                    throw std::runtime_error(
                        "RmlUi smoke renderer did not receive an RGBA8+D24S8 framebuffer");
                }
                frame.GraphicsCommandList->clearTextureFloat(
                    activeRenderTarget.Color,
                    nvrhi::AllSubresources,
                    nvrhi::Color(0.f, 0.f, 0.f, 0.f));
                frame.GraphicsCommandList->clearDepthStencilTexture(
                    activeRenderTarget.DepthStencil,
                    nvrhi::AllSubresources,
                    true,
                    1.f,
                    true,
                    bInverseClipFrame ? 1 : 0);
                rmlManager.BeginGpuFrame(
                    frame.GraphicsCommandList,
                    activeRenderTarget.Framebuffer,
                    frame.FrameIndex,
                    frame.CompletedSubmissionInstance);

                if (bReleased && !bRecycled && frameNumber >= 5)
                {
                    directResources = CreateDirectResources(renderInterface);
                    if (directResources.Geometry == retiredGeometry
                        || directResources.Texture == retiredTexture)
                    {
                        throw std::runtime_error("RmlUi retired opaque handle generation was reused unchanged");
                    }
                    bRecycled = true;
                }

                if (bInverseClipFrame)
                {
                    ExerciseInverseClipMask(renderInterface, directResources);
                }
                else
                {
                    if (frameNumber == 0)
                    {
                        RenderDirectResources(renderInterface, directResources);
                        const HEngine::NvrhiRmlRenderBackendStats& retryStats =
                            rmlManager.GetRenderStats();
                        if (retryStats.GeometryUploads <= abortedGeometryUploads
                            || retryStats.TextureUploads <= abortedTextureUploads)
                        {
                            throw std::runtime_error(
                                "RmlUi aborted first-use uploads were not recorded again");
                        }
                    }
                    rmlManager.Render();
                    if (frameNumber == 0)
                    {
                        const HEngine::NvrhiRmlRenderBackendStats& documentStats =
                            rmlManager.GetRenderStats();
                        if (documentStats.CachedPipelines < 4
                            || documentStats.UnsupportedFeatureRequests != 0)
                        {
                            throw std::runtime_error(
                                "Nested rounded RML clipping did not exercise Set and Intersect masks");
                        }
                        ExerciseAdvancedRenderInterface(renderInterface, directResources);
                    }
                }
                if (!bReleased || bRecycled)
                    RenderDirectResources(renderInterface, directResources);

                if (frameNumber == 1)
                {
                    retiredGeometry = directResources.Geometry;
                    retiredTexture = directResources.Texture;
                    ReleaseDirectResources(renderInterface, directResources);
                    bReleased = true;
                }

                rmlManager.EndGpuFrame();
                renderDevice.SubmitAndPresent(frame, false);
                rmlManager.NotifyGpuFrameSubmitted(
                    frame.FrameIndex,
                    frame.SubmissionInstance);
                HEngine::PresentationClock::GetInstance().CompleteFrame();
            }
            catch (...)
            {
                if (rmlManager.IsGpuFrameActive())
                {
                    try
                    {
                        rmlManager.EndGpuFrame();
                    }
                    catch (...)
                    {
                    }
                }
                if (frame.bSubmissionIssued)
                {
                    rmlManager.NotifyGpuFrameSubmitted(
                        frame.FrameIndex,
                        frame.SubmissionInstance);
                }
                else
                {
                    rmlManager.AbortGpuFrame(frame.FrameIndex);
                    renderDevice.AbortFrame(frame);
                }
                throw;
            }
        }

        const HEngine::NvrhiRmlRenderBackendStats stats = rmlManager.GetRenderStats();
        if (completionListener.AnimationEndCount == 0 || completionListener.TransitionEndCount == 0)
            throw std::runtime_error("RmlUi animation/transition completion bubbling was not exercised");
        if (!bRecycled || stats.GeometrySlotsReused == 0 || stats.TextureSlotsReused == 0)
            throw std::runtime_error("RmlUi deferred geometry/texture slot recycling was not exercised");
        if (stats.DrawCalls == 0 || stats.RenderedTriangles == 0
            || stats.GeometryUploads == 0 || stats.TextureUploads < 2
            || stats.InvalidGeometryHandles != 0 || stats.InvalidTextureHandles != 0
            || stats.CachedPipelines < 4 || stats.UnsupportedFeatureRequests != 0
            || stats.LayerComposites == 0 || stats.FilterPasses == 0
            || stats.CustomShaderDraws == 0)
        {
            throw std::runtime_error("RmlUi smoke observed invalid renderer statistics");
        }

        const HEngine::RmlRenderCapabilities capabilities = rmlManager.GetRenderCapabilities();
        if (!capabilities.ClipMask || !capabilities.ClipMaskRequiresStencilAttachment
            || !capabilities.Layers || !capabilities.Filters || !capabilities.MaskImage
            || !capabilities.CustomShaders)
        {
            throw std::runtime_error(
                "RmlUi smoke renderer did not report clip-mask/layer/filter capabilities");
        }

        ReleaseDirectResources(renderInterface, directResources);
        renderDevice.WaitForIdle();
        rmlManager.Shutdown();
        context = nullptr;
        document = nullptr;
        smokeRenderTarget = {};
        inverseSmokeRenderTarget = {};
        renderDevice.Shutdown();
        window.reset();

        std::cout
            << "RML_DX12_SMOKE_PASS: document geometry, generated texture, scissor, "
            << "SVG, Lottie, document Set/Intersect plus explicit SetInverse clip masks on "
            << "RGBA8+D24S8, layer compositing, opacity/blur/saturate filters, completion "
            << "events, mask images, whitelisted custom shaders, frame-safe generation "
            << "recycling, aborted-upload rollback, resize, validation, and shutdown\n";
        result = 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "RML_DX12_SMOKE_FAIL: " << exception.what() << '\n';
        if (rmlManager.IsGpuFrameActive())
        {
            try
            {
                rmlManager.EndGpuFrame();
            }
            catch (...)
            {
            }
        }
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
        if (rmlManager.IsInitialized())
        {
            try
            {
                rmlManager.Shutdown();
            }
            catch (...)
            {
            }
        }
        context = nullptr;
        document = nullptr;
        smokeRenderTarget = {};
        inverseSmokeRenderTarget = {};
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
