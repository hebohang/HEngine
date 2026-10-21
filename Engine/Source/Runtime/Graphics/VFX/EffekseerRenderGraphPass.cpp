#include "Hepch.h"

#include "EffekseerRenderGraphPass.h"

#include "Runtime/Graphics/RHI/PipelineCache.h"
#include "Runtime/Graphics/RHI/ShaderArtifactResolver.h"
#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace HEngine
{
    void EffekseerRenderGraphPass::AddSimulationToGraph(RenderGraph& graph)
    {
        struct SimulationPassData {};
        graph.AddPass<SimulationPassData>("VFXSimulation",
            [](RenderGraphBuilder& builder, SimulationPassData&) { builder.SetSideEffect(); },
            [](RenderGraphContext& context, const SimulationPassData&)
            {
                EffekseerRuntime::ComputeParticles(context.GetCommandList());
            });
    }

    namespace
    {
        struct DepthSnapshotPassData
        {
            TextureHandle SceneDepth;
            TextureHandle DepthSnapshot;
        };

        struct BackgroundCopyPassData
        {
            TextureHandle SceneColor;
            TextureHandle BackgroundSnapshot;
        };

        struct VfxTransparentPassData
        {
            TextureHandle SceneColor;
            TextureHandle SceneDepth;
            TextureHandle DepthSnapshot;
            TextureHandle BackgroundSnapshot;
        };

        struct VfxFramebufferKey
        {
            nvrhi::ITexture* Color = nullptr;
            nvrhi::ITexture* Depth = nullptr;

            [[nodiscard]] bool operator==(const VfxFramebufferKey&) const = default;
        };

        [[nodiscard]] uint64_t MixKey(uint64_t seed, uint64_t value) noexcept
        {
            seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
            return seed == 0 ? 1 : seed;
        }

        struct VfxFramebufferKeyHash
        {
            [[nodiscard]] size_t operator()(const VfxFramebufferKey& key) const noexcept
            {
                uint64_t value = reinterpret_cast<uintptr_t>(key.Color);
                value = MixKey(value, reinterpret_cast<uintptr_t>(key.Depth));
                return static_cast<size_t>(value);
            }
        };

        [[nodiscard]] std::string ReadShaderContentHash(const std::filesystem::path& path)
        {
            std::ifstream stream(path);
            if (!stream)
                throw std::runtime_error(
                    "Failed to open Effekseer shader cache-key sidecar: " +
                    Utils::PathToUtf8(path));

            std::string line;
            while (std::getline(stream, line))
            {
                constexpr std::string_view c_KeyPrefix = "key=";
                if (line.starts_with(c_KeyPrefix) && line.size() > c_KeyPrefix.size())
                    return line.substr(c_KeyPrefix.size());
            }
            throw std::runtime_error(
                "Effekseer shader cache-key sidecar has no content hash: " +
                Utils::PathToUtf8(path));
        }

        [[nodiscard]] RenderGraphTextureDesc MakeSnapshotDesc(
            const char* debugName,
            uint32_t width,
            uint32_t height,
            nvrhi::Format format)
        {
            RenderGraphTextureDesc desc;
            desc.DebugName = debugName;
            desc.Width = width;
            desc.Height = height;
            desc.Format = format;
            desc.Dimension = nvrhi::TextureDimension::Texture2D;
            return desc;
        }
    }

    struct EffekseerRenderGraphPass::Impl
    {
        explicit Impl(const EffekseerRenderGraphPassDesc& desc)
            : Device(desc.Device)
            , ShaderManagerInstance(desc.Device)
            , ShaderArtifacts(desc.ShaderCacheDirectory, desc.Device)
            , PipelineCacheInstance(desc.Device)
        {
            if (!Device)
                throw std::invalid_argument(
                    "EffekseerRenderGraphPass requires a valid NVRHI device");
            if (desc.ShaderCacheDirectory.empty())
                throw std::invalid_argument(
                    "EffekseerRenderGraphPass requires the shader cache directory");

            const std::filesystem::path vertexPath =
                desc.ShaderCacheDirectory / "EffekseerDepthSnapshot.vs";
            const std::filesystem::path pixelPath =
                desc.ShaderCacheDirectory / "EffekseerDepthSnapshot.ps";
            DepthVertexShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/EffekseerDepthSnapshot.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Vertex,
                    .EntryPoint = "VS",
                    .TargetProfile = "vs_6_0",
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(vertexPath, ".cachekey")),
                },
                vertexPath,
                "Effekseer depth snapshot VS");
            DepthPixelShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/EffekseerDepthSnapshot.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = "ps_6_0",
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(pixelPath, ".cachekey")),
                },
                pixelPath,
                "Effekseer depth snapshot PS");

            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc
                .setVisibility(nvrhi::ShaderType::Pixel)
                .setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
            DepthBindingLayout = PipelineCacheInstance.GetOrCreateBindingLayout(
                BindingLayoutKey{ 0x564658445054484cull },
                layoutDesc,
                "Effekseer space3 depth snapshot layout");
        }

        [[nodiscard]] nvrhi::IFramebuffer* GetOrCreateDepthFramebuffer(
            nvrhi::ITexture* depthSnapshot)
        {
            if (const auto found = DepthFramebuffers.find(depthSnapshot);
                found != DepthFramebuffers.end())
            {
                return found->second;
            }

            nvrhi::FramebufferDesc desc;
            desc.addColorAttachment(depthSnapshot);
            nvrhi::FramebufferHandle framebuffer = Device->createFramebuffer(desc);
            if (!framebuffer)
                throw std::runtime_error(
                    "NVRHI failed to create the Effekseer R32 depth-snapshot framebuffer");
            return DepthFramebuffers.emplace(depthSnapshot, std::move(framebuffer)).first->second;
        }

        [[nodiscard]] nvrhi::IBindingSet* GetOrCreateDepthBindingSet(nvrhi::ITexture* sceneDepth)
        {
            if (const auto found = DepthBindingSets.find(sceneDepth);
                found != DepthBindingSets.end())
            {
                return found->second;
            }

            nvrhi::BindingSetDesc desc;
            // Let NVRHI select the depth SRV for each backend. Forcing R32
            // creates an invalid color-aspect view of a Vulkan D32 image.
            desc.addItem(nvrhi::BindingSetItem::Texture_SRV(
                0,
                sceneDepth));
            nvrhi::BindingSetHandle bindingSet = Device->createBindingSet(desc, DepthBindingLayout);
            if (!bindingSet)
                throw std::runtime_error(
                    "NVRHI failed to create the Effekseer depth-snapshot binding set");
            return DepthBindingSets.emplace(sceneDepth, std::move(bindingSet)).first->second;
        }

        [[nodiscard]] nvrhi::IGraphicsPipeline* GetOrCreateDepthPipeline(
            nvrhi::IFramebuffer* framebuffer)
        {
            nvrhi::GraphicsPipelineDesc desc;
            desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setVertexShader(DepthVertexShader)
                .setPixelShader(DepthPixelShader)
                .addBindingLayout(DepthBindingLayout);
            desc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
            desc.renderState.rasterState.setCullNone().enableDepthClip();
            return PipelineCacheInstance.GetOrCreateGraphicsPipeline(
                GraphicsPipelineKey{ 0x5646584450544850ull },
                desc,
                framebuffer->getFramebufferInfo(),
                "Effekseer fullscreen D32-to-R32 snapshot pipeline");
        }

        [[nodiscard]] nvrhi::IFramebuffer* GetOrCreateVfxFramebuffer(
            nvrhi::ITexture* sceneColor,
            nvrhi::ITexture* sceneDepth)
        {
            const VfxFramebufferKey key{ sceneColor, sceneDepth };
            if (const auto found = VfxFramebuffers.find(key); found != VfxFramebuffers.end())
                return found->second;

            nvrhi::FramebufferDesc desc;
            desc.addColorAttachment(sceneColor)
                .setDepthAttachment(nvrhi::FramebufferAttachment()
                    .setTexture(sceneDepth)
                    .setReadOnly(true));
            nvrhi::FramebufferHandle framebuffer = Device->createFramebuffer(desc);
            if (!framebuffer)
                throw std::runtime_error("NVRHI failed to create the Effekseer VFX framebuffer");
            return VfxFramebuffers.emplace(key, std::move(framebuffer)).first->second;
        }

        void RecordDepthSnapshot(
            RenderGraphContext& context,
            const DepthSnapshotPassData& data,
            uint32_t width,
            uint32_t height)
        {
            nvrhi::ITexture* sceneDepth = context.GetTexture(
                data.SceneDepth,
                RenderGraphAccess::Read);
            nvrhi::ITexture* depthSnapshot = context.GetTexture(
                data.DepthSnapshot,
                RenderGraphAccess::Write);
            nvrhi::IFramebuffer* framebuffer = GetOrCreateDepthFramebuffer(depthSnapshot);
            nvrhi::IBindingSet* bindingSet = GetOrCreateDepthBindingSet(sceneDepth);
            nvrhi::IGraphicsPipeline* pipeline = GetOrCreateDepthPipeline(framebuffer);

            nvrhi::GraphicsState state;
            state.setPipeline(pipeline)
                .setFramebuffer(framebuffer)
                .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                    nvrhi::Viewport(static_cast<float>(width), static_cast<float>(height))))
                .addBindingSet(bindingSet);
            context.GetCommandList()->setGraphicsState(state);
            context.GetCommandList()->draw(nvrhi::DrawArguments().setVertexCount(3));
        }

        void ClearResourceCaches()
        {
            DepthBindingSets.clear();
            DepthFramebuffers.clear();
            VfxFramebuffers.clear();
        }

        EffekseerRenderView View;

        nvrhi::DeviceHandle Device;
        ShaderManager ShaderManagerInstance;
        ShaderArtifactResolver ShaderArtifacts;
        PipelineCache PipelineCacheInstance;
        nvrhi::ShaderHandle DepthVertexShader;
        nvrhi::ShaderHandle DepthPixelShader;
        nvrhi::BindingLayoutHandle DepthBindingLayout;
        std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> DepthBindingSets;
        std::unordered_map<nvrhi::ITexture*, nvrhi::FramebufferHandle> DepthFramebuffers;
        std::unordered_map<VfxFramebufferKey, nvrhi::FramebufferHandle, VfxFramebufferKeyHash>
            VfxFramebuffers;
    };

    EffekseerRenderGraphPass::EffekseerRenderGraphPass(
        const EffekseerRenderGraphPassDesc& desc)
        : mImpl(std::make_unique<Impl>(desc))
    {
    }

    EffekseerRenderGraphPass::~EffekseerRenderGraphPass() = default;

    EffekseerRenderGraphPassResult EffekseerRenderGraphPass::AddToGraph(
        RenderGraph& graph,
        const EffekseerRenderGraphInputs& inputs,
        const VFXRenderRequirements& requirements,
        bool bRunEmptyPass)
    {
        EffekseerRenderGraphPassResult result;
        if (!requirements.bHasActiveEffects && !bRunEmptyPass)
            return result;
        if (EffekseerRuntime::GetRenderBackendStatus() == VFXRenderBackendStatus::Unconfigured)
        {
            throw RenderGraphValidationError(
                "Effekseer VFX pass was requested before EffekseerRuntime::Initialize");
        }
        if (!inputs.SceneColor || !inputs.SceneDepth)
        {
            throw RenderGraphValidationError(
                "Effekseer VFX pass requires valid SceneColor and SceneDepth handles.");
        }
        if (inputs.Width == 0 || inputs.Height == 0)
        {
            throw RenderGraphValidationError(
                "Effekseer VFX pass requires non-zero render dimensions.");
        }

        Impl* const impl = mImpl.get();
        impl->View = EffekseerRenderView{
            .View = inputs.View,
            .Projection = inputs.Projection,
            .CameraPosition = inputs.CameraPosition,
            .ZNear = inputs.ZNear,
            .ZFar = inputs.ZFar,
            .LightDirection = inputs.LightDirection,
            .LightColor = inputs.LightColor,
            .LightAmbientColor = inputs.LightAmbientColor,
        };
        const nvrhi::Format sceneColorFormat = graph.GetTextureDesc(inputs.SceneColor).Format;
        if (requirements.bRequiresDepthSnapshot)
        {
            result.DepthSnapshotPass = graph.AddPass<DepthSnapshotPassData>(
                "VFXDepthSnapshot",
                RenderGraphQueue::Graphics,
                [&result, inputs](RenderGraphBuilder& builder, DepthSnapshotPassData& data)
                {
                    // ShaderResource and DepthRead have mutually exclusive DX12
                    // enhanced-barrier layouts. The conversion pass samples the
                    // depth texture only; VFXTransparent transitions it back to
                    // DepthRead before binding the read-only DSV.
                    data.SceneDepth = builder.ShaderResource(inputs.SceneDepth);
                    data.DepthSnapshot = builder.CreateTexture(MakeSnapshotDesc(
                        "Effekseer R32 Float Depth Snapshot",
                        inputs.Width,
                        inputs.Height,
                        nvrhi::Format::R32_FLOAT));
                    data.DepthSnapshot = builder.RenderTarget(data.DepthSnapshot);
                    result.DepthSnapshot = data.DepthSnapshot;
                },
                [impl, width = inputs.Width, height = inputs.Height](
                    RenderGraphContext& context,
                    const DepthSnapshotPassData& data)
                {
                    impl->RecordDepthSnapshot(context, data, width, height);
                });
            result.bDepthSnapshotAdded = true;
        }

        if (requirements.bRequiresBackgroundSnapshot)
        {
            result.BackgroundCopyPass = graph.AddPass<BackgroundCopyPassData>(
                "VFXBackgroundCopy",
                RenderGraphQueue::Graphics,
                [&result, inputs, sceneColorFormat](
                    RenderGraphBuilder& builder,
                    BackgroundCopyPassData& data)
                {
                    data.SceneColor = builder.CopySource(inputs.SceneColor);
                    data.BackgroundSnapshot = builder.CreateTexture(MakeSnapshotDesc(
                        "Effekseer HDR Background Snapshot",
                        inputs.Width,
                        inputs.Height,
                        sceneColorFormat));
                    data.BackgroundSnapshot = builder.CopyDest(data.BackgroundSnapshot);
                    result.BackgroundSnapshot = data.BackgroundSnapshot;
                },
                [](RenderGraphContext& context, const BackgroundCopyPassData& data)
                {
                    nvrhi::ITexture* source = context.GetTexture(
                        data.SceneColor,
                        RenderGraphAccess::Read);
                    nvrhi::ITexture* destination = context.GetTexture(
                        data.BackgroundSnapshot,
                        RenderGraphAccess::Write);
                    context.GetCommandList()->copyTexture(
                        destination,
                        nvrhi::TextureSlice(),
                        source,
                        nvrhi::TextureSlice());
                });
            result.bBackgroundCopyAdded = true;
        }

        const TextureHandle depthSnapshot = result.DepthSnapshot;
        const TextureHandle backgroundSnapshot = result.BackgroundSnapshot;
        result.VfxPass = graph.AddPass<VfxTransparentPassData>(
            "VFXTransparent",
            RenderGraphQueue::Graphics,
            [inputs,
             requirements,
             depthSnapshot,
             backgroundSnapshot](RenderGraphBuilder& builder, VfxTransparentPassData& data)
            {
                data.SceneColor = builder.ReadWrite(
                    inputs.SceneColor,
                    RenderGraphUsage::RenderTarget);
                data.SceneDepth = builder.DepthRead(inputs.SceneDepth);
                if (requirements.bRequiresDepthSnapshot)
                    data.DepthSnapshot = builder.ShaderResource(depthSnapshot);
                if (requirements.bRequiresBackgroundSnapshot)
                    data.BackgroundSnapshot = builder.ShaderResource(backgroundSnapshot);
            },
            [impl](RenderGraphContext& context, const VfxTransparentPassData& data)
            {
                nvrhi::ITexture* sceneColor = context.GetTexture(
                    data.SceneColor,
                    RenderGraphAccess::ReadWrite);
                nvrhi::ITexture* sceneDepth = context.GetTexture(
                    data.SceneDepth,
                    RenderGraphAccess::Read);

                const EffekseerRenderView view = impl->View;
                EffekseerRuntime::SetCamera(view.View, view.Projection);
                EffekseerRuntime::Render(EffekseerRenderDesc{
                    .CommandList = context.GetCommandList(),
                    .Framebuffer = impl->GetOrCreateVfxFramebuffer(sceneColor, sceneDepth),
                    .DepthSampleTexture = data.DepthSnapshot
                        ? context.GetTexture(data.DepthSnapshot, RenderGraphAccess::Read)
                        : nullptr,
                    .BackgroundTexture = data.BackgroundSnapshot
                        ? context.GetTexture(data.BackgroundSnapshot, RenderGraphAccess::Read)
                        : nullptr,
                    .ZNear = view.ZNear,
                    .ZFar = view.ZFar,
                    .CameraPosition = view.CameraPosition,
                    .LightDirection = view.LightDirection,
                    .LightColor = view.LightColor,
                    .LightAmbientColor = view.LightAmbientColor,
                });
            });
        result.bAdded = true;
        return result;
    }

    void EffekseerRenderGraphPass::UpdateView(const EffekseerRenderView& view)
    {
        mImpl->View = view;
    }

    void EffekseerRenderGraphPass::ClearResourceCaches()
    {
        mImpl->ClearResourceCaches();
    }

    std::function<void()> EffekseerRenderGraphPass::CaptureGraphState()
    {
        return [impl = mImpl.get(), depthBindings = mImpl->DepthBindingSets,
            depthFramebuffers = mImpl->DepthFramebuffers,
            vfxFramebuffers = mImpl->VfxFramebuffers, view = mImpl->View]() mutable noexcept
        {
            impl->DepthBindingSets.swap(depthBindings);
            impl->DepthFramebuffers.swap(depthFramebuffers);
            impl->VfxFramebuffers.swap(vfxFramebuffers);
            std::swap(impl->View, view);
        };
    }

    void EffekseerRenderGraphPass::ClearGraphResourceCaches() noexcept
    {
        mImpl->DepthBindingSets.clear();
        mImpl->DepthFramebuffers.clear();
        mImpl->VfxFramebuffers.clear();
    }

    size_t EffekseerRenderGraphPass::GetFramebufferCacheSize() const noexcept
    {
        return mImpl->DepthFramebuffers.size() + mImpl->VfxFramebuffers.size();
    }

    size_t EffekseerRenderGraphPass::GetDepthBindingSetCacheSize() const noexcept
    {
        return mImpl->DepthBindingSets.size();
    }
}
