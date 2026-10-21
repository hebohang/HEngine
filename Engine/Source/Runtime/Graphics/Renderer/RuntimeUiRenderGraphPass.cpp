#include "Hepch.h"

#include "RuntimeUiRenderGraphPass.h"

#include "Runtime/Graphics/RenderGraph/TextureResolvePass.h"
#include "Runtime/Graphics/RHI/PipelineCache.h"
#include "Runtime/Graphics/RHI/ShaderArtifactResolver.h"
#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace HEngine
{
    namespace
    {
        constexpr nvrhi::Format c_UiColorFormat = nvrhi::Format::RGBA8_UNORM;
        constexpr nvrhi::Format c_UiStencilFormat = nvrhi::Format::D24S8;
        constexpr uint32_t c_UiSampleCount = 2;

        struct RuntimeUiRenderPassData
        {
            TextureHandle Backdrop;
            TextureHandle UiColor;
            TextureHandle UiStencil;
        };

        struct UiTransitionPassData
        {
            TextureHandle UiColor;
            TextureHandle TransitionColor;
        };

        struct UiCompositePassData
        {
            TextureHandle TransitionColor;
            TextureHandle BackBuffer;
        };

        struct UiFramebufferKey
        {
            nvrhi::ITexture* Color = nullptr;
            nvrhi::ITexture* Stencil = nullptr;

            [[nodiscard]] bool operator==(const UiFramebufferKey&) const = default;
        };

        [[nodiscard]] uint64_t MixKey(uint64_t seed, uint64_t value) noexcept
        {
            seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
            return seed == 0 ? 1 : seed;
        }

        struct UiFramebufferKeyHash
        {
            [[nodiscard]] size_t operator()(const UiFramebufferKey& key) const noexcept
            {
                uint64_t value = reinterpret_cast<uintptr_t>(key.Color);
                value = MixKey(value, reinterpret_cast<uintptr_t>(key.Stencil));
                return static_cast<size_t>(value);
            }
        };

        struct UiFramebufferCacheEntry
        {
            nvrhi::FramebufferHandle Framebuffer;
            uint64_t LastGraphGeneration = 0;
            // A slot remains here until BeginFrame proves its previous
            // submission complete. Keeping one bit-equivalent set per entry
            // avoids releasing NVRHI objects still referenced by that slot.
            std::unordered_set<uint32_t> PendingFrameSlots;
        };

        struct UiBindingSetCacheEntry
        {
            nvrhi::BindingSetHandle BindingSet;
            uint64_t LastGraphGeneration = 0;
            std::unordered_set<uint32_t> PendingFrameSlots;
        };

        struct alignas(16) UiCompositeConstants
        {
            uint32_t PassKind = 0;
            uint32_t Effect = 0;
            uint32_t Flags = 0;
            float Progress = 1.0f;
            float InverseWidth = 1.0f;
            float InverseHeight = 1.0f;
            float EdgeSoftnessPixels = 2.0f;
            float HalftoneCellPixels = 24.0f;
            float CenterX = 0.5f;
            float CenterY = 0.5f;
            float Padding0 = 0.0f;
            float Padding1 = 0.0f;
        };

        static_assert(sizeof(UiCompositeConstants) == 48);

        [[nodiscard]] std::string ReadShaderContentHash(const std::filesystem::path& path)
        {
            std::ifstream stream(path);
            if (!stream)
            {
                throw std::runtime_error(
                    "Failed to open runtime UI shader cache-key sidecar: " +
                    Utils::PathToUtf8(path));
            }

            std::string line;
            while (std::getline(stream, line))
            {
                constexpr std::string_view c_KeyPrefix = "key=";
                if (line.starts_with(c_KeyPrefix) && line.size() > c_KeyPrefix.size())
                    return line.substr(c_KeyPrefix.size());
            }
            throw std::runtime_error(
                "Runtime UI shader cache-key sidecar has no content hash: " +
                Utils::PathToUtf8(path));
        }

        [[nodiscard]] RenderGraphTextureDesc MakeUiTextureDesc(
            const char* debugName,
            uint32_t width,
            uint32_t height,
            nvrhi::Format format,
            uint32_t sampleCount = 1)
        {
            RenderGraphTextureDesc desc;
            desc.DebugName = debugName;
            desc.Width = width;
            desc.Height = height;
            desc.SampleCount = sampleCount;
            desc.Format = format;
            desc.Dimension = nvrhi::TextureDimension::Texture2D;
            desc.ClearValue = format == c_UiStencilFormat
                ? nvrhi::Color(1.0f, 0.0f, 0.0f, 0.0f)
                : nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f);
            desc.bUseClearValue = true;
            return desc;
        }

        [[nodiscard]] float FiniteOr(float value, float fallback) noexcept
        {
            return std::isfinite(value) ? value : fallback;
        }

        [[nodiscard]] UiCompositeConstants MakeConstants(
            const RuntimeUiTransitionParameters& parameters,
            uint32_t width,
            uint32_t height,
            bool bComposite) noexcept
        {
            const float progress = std::clamp(FiniteOr(parameters.Progress, 1.0f), 0.0f, 1.0f);
            const float edgeSoftness = std::clamp(
                FiniteOr(parameters.EdgeSoftnessPixels, 2.0f),
                0.5f,
                64.0f);
            const float halftoneCell = std::clamp(
                FiniteOr(parameters.HalftoneCellPixels, 24.0f),
                4.0f,
                128.0f);

            uint32_t effect = static_cast<uint32_t>(parameters.Effect);
            if (effect > static_cast<uint32_t>(RuntimeUiTransitionEffect::HalftoneDotReveal))
                effect = static_cast<uint32_t>(RuntimeUiTransitionEffect::None);

            uint32_t flags = 0;
            if (parameters.bReduceMotion)
                flags |= 1u;
            if (parameters.bReduceFlash)
                flags |= 2u;

            return UiCompositeConstants{
                .PassKind = bComposite ? 1u : 0u,
                .Effect = effect,
                .Flags = flags,
                .Progress = progress,
                .InverseWidth = 1.0f / static_cast<float>(std::max(width, 1u)),
                .InverseHeight = 1.0f / static_cast<float>(std::max(height, 1u)),
                .EdgeSoftnessPixels = edgeSoftness,
                .HalftoneCellPixels = halftoneCell,
                .CenterX = std::clamp(FiniteOr(parameters.CenterX, 0.5f), 0.0f, 1.0f),
                .CenterY = std::clamp(FiniteOr(parameters.CenterY, 0.5f), 0.0f, 1.0f),
            };
        }

        [[nodiscard]] uint64_t MakePipelineKey(
            uint64_t seed,
            const nvrhi::FramebufferInfo& info) noexcept
        {
            for (nvrhi::Format format : info.colorFormats)
                seed = MixKey(seed, static_cast<uint64_t>(format));
            seed = MixKey(seed, static_cast<uint64_t>(info.depthFormat));
            seed = MixKey(seed, info.sampleCount);
            return MixKey(seed, info.sampleQuality);
        }
    }

    struct RuntimeUiRenderGraphPass::Impl
    {
        explicit Impl(const RuntimeUiRenderGraphPassDesc& desc)
            : Device(desc.Device)
            , ShaderManagerInstance(desc.Device)
            , ShaderArtifacts(desc.ShaderCacheDirectory, desc.Device)
            , PipelineCacheInstance(desc.Device)
        {
            if (!Device)
            {
                throw std::invalid_argument(
                    "RuntimeUiRenderGraphPass requires a valid NVRHI device");
            }
            if (desc.ShaderCacheDirectory.empty())
            {
                throw std::invalid_argument(
                    "RuntimeUiRenderGraphPass requires the shader cache directory");
            }

            const std::filesystem::path vertexPath =
                desc.ShaderCacheDirectory / "RuntimeUiComposite.vs";
            const std::filesystem::path pixelPath =
                desc.ShaderCacheDirectory / "RuntimeUiComposite.ps";
            VertexShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/RuntimeUiComposite.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Vertex,
                    .EntryPoint = "VS",
                    .TargetProfile = "vs_6_0",
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(vertexPath, ".cachekey")),
                },
                vertexPath,
                "Runtime UI fullscreen VS");
            PixelShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/RuntimeUiComposite.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = "ps_6_0",
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(pixelPath, ".cachekey")),
                },
                pixelPath,
                "Runtime UI transition/composite PS");

            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc
                .setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
                .setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(
                    0, sizeof(UiCompositeConstants)))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Sampler(0));
            BindingLayout = PipelineCacheInstance.GetOrCreateBindingLayout(
                BindingLayoutKey{ 0x52545549434f4d4cull },
                layoutDesc,
                "Runtime UI space3 transition/composite layout");

            nvrhi::SamplerDesc samplerDesc;
            samplerDesc.setAllFilters(true)
                .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
            Sampler = Device->createSampler(samplerDesc);
            if (!Sampler)
                throw std::runtime_error("NVRHI failed to create runtime UI linear clamp sampler");
        }

        [[nodiscard]] nvrhi::IFramebuffer* GetOrCreateUiFramebuffer(
            nvrhi::ITexture* color,
            nvrhi::ITexture* stencil,
            uint64_t graphGeneration,
            uint32_t frameSlot)
        {
            const UiFramebufferKey key{ color, stencil };
            if (const auto found = UiFramebuffers.find(key); found != UiFramebuffers.end())
            {
                found->second.LastGraphGeneration = graphGeneration;
                found->second.PendingFrameSlots.insert(frameSlot);
                return found->second.Framebuffer;
            }

            nvrhi::FramebufferDesc desc;
            desc.addColorAttachment(color).setDepthAttachment(stencil);
            nvrhi::FramebufferHandle framebuffer = Device->createFramebuffer(desc);
            if (!framebuffer)
            {
                throw std::runtime_error(
                    "NVRHI failed to create runtime UI RGBA8+D24S8 framebuffer");
            }
            UiFramebufferCacheEntry entry{
                .Framebuffer = std::move(framebuffer),
                .LastGraphGeneration = graphGeneration,
            };
            entry.PendingFrameSlots.insert(frameSlot);
            return UiFramebuffers.emplace(key, std::move(entry)).first->second.Framebuffer;
        }

        [[nodiscard]] nvrhi::IFramebuffer* GetOrCreateColorFramebuffer(
            nvrhi::ITexture* color,
            uint64_t graphGeneration,
            uint32_t frameSlot)
        {
            if (const auto found = ColorFramebuffers.find(color);
                found != ColorFramebuffers.end())
            {
                found->second.LastGraphGeneration = graphGeneration;
                found->second.PendingFrameSlots.insert(frameSlot);
                return found->second.Framebuffer;
            }

            nvrhi::FramebufferDesc desc;
            desc.addColorAttachment(color);
            nvrhi::FramebufferHandle framebuffer = Device->createFramebuffer(desc);
            if (!framebuffer)
                throw std::runtime_error("NVRHI failed to create runtime UI color framebuffer");
            UiFramebufferCacheEntry entry{
                .Framebuffer = std::move(framebuffer),
                .LastGraphGeneration = graphGeneration,
            };
            entry.PendingFrameSlots.insert(frameSlot);
            return ColorFramebuffers.emplace(color, std::move(entry)).first->second.Framebuffer;
        }

        [[nodiscard]] nvrhi::IBindingSet* GetOrCreateBindingSet(
            nvrhi::ITexture* texture,
            uint64_t graphGeneration,
            uint32_t frameSlot)
        {
            if (const auto found = BindingSets.find(texture); found != BindingSets.end())
            {
                found->second.LastGraphGeneration = graphGeneration;
                found->second.PendingFrameSlots.insert(frameSlot);
                return found->second.BindingSet;
            }

            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::PushConstants(
                    0, sizeof(UiCompositeConstants)))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture))
                .addItem(nvrhi::BindingSetItem::Sampler(0, Sampler));
            nvrhi::BindingSetHandle bindingSet = Device->createBindingSet(desc, BindingLayout);
            if (!bindingSet)
            {
                throw std::runtime_error(
                    "NVRHI failed to create runtime UI transition/composite binding set");
            }
            UiBindingSetCacheEntry entry{
                .BindingSet = std::move(bindingSet),
                .LastGraphGeneration = graphGeneration,
            };
            entry.PendingFrameSlots.insert(frameSlot);
            return BindingSets.emplace(texture, std::move(entry)).first->second.BindingSet;
        }

        [[nodiscard]] nvrhi::IGraphicsPipeline* GetOrCreatePipeline(
            nvrhi::IFramebuffer* framebuffer,
            bool bComposite)
        {
            const nvrhi::FramebufferInfo info = framebuffer->getFramebufferInfo();
            nvrhi::GraphicsPipelineDesc desc;
            desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setVertexShader(VertexShader)
                .setPixelShader(PixelShader)
                .addBindingLayout(BindingLayout);
            desc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
            desc.renderState.rasterState.setCullNone().enableDepthClip();
            if (bComposite)
            {
                // The source texture is already premultiplied. Preserve the
                // destination alpha contract for offscreen and capture users.
                desc.renderState.blendState.targets[0]
                    .enableBlend()
                    .setSrcBlend(nvrhi::BlendFactor::One)
                    .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                    .setBlendOp(nvrhi::BlendOp::Add)
                    .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                    .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
                    .setBlendOpAlpha(nvrhi::BlendOp::Add);
            }

            const uint64_t baseKey = bComposite
                ? 0x52545549434f4d50ull
                : 0x525455495452414eull;
            return PipelineCacheInstance.GetOrCreateGraphicsPipeline(
                GraphicsPipelineKey{ MakePipelineKey(baseKey, info) },
                desc,
                info,
                bComposite
                    ? "Runtime UI premultiplied composite pipeline"
                    : "Runtime UI fullscreen transition pipeline");
        }

        void RecordFullscreen(
            RenderGraphContext& context,
            nvrhi::ITexture* source,
            nvrhi::ITexture* destination,
            uint32_t width,
            uint32_t height,
            bool bComposite,
            uint64_t graphGeneration)
        {
            nvrhi::IFramebuffer* framebuffer = GetOrCreateColorFramebuffer(
                destination,
                graphGeneration,
                Frame.FrameSlot);
            nvrhi::IBindingSet* bindingSet = GetOrCreateBindingSet(
                source,
                graphGeneration,
                Frame.FrameSlot);
            nvrhi::GraphicsState state;
            state.setPipeline(GetOrCreatePipeline(framebuffer, bComposite))
                .setFramebuffer(framebuffer)
                .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                    nvrhi::Viewport(static_cast<float>(width), static_cast<float>(height))))
                .addBindingSet(bindingSet);

            nvrhi::ICommandList* commandList = context.GetCommandList();
            commandList->setGraphicsState(state);
            const UiCompositeConstants constants = MakeConstants(
                Frame.Transition,
                width,
                height,
                bComposite);
            commandList->setPushConstants(&constants, sizeof(constants));
            commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
        }

        template<typename CacheType>
        void RetireSafeFrameSlot(CacheType& cache, uint32_t frameSlot)
        {
            for (auto iterator = cache.begin(); iterator != cache.end();)
            {
                auto& entry = iterator->second;
                if (entry.LastGraphGeneration == GraphGeneration)
                {
                    // Current-graph entries must remain warm. Their slot set is
                    // the retirement fence if a later AddToGraph supersedes them.
                    ++iterator;
                    continue;
                }

                entry.PendingFrameSlots.erase(frameSlot);
                if (entry.PendingFrameSlots.empty())
                    iterator = cache.erase(iterator);
                else
                    ++iterator;
            }
        }

        void RetireSafeFrameSlot(uint32_t frameSlot)
        {
            RetireSafeFrameSlot(UiFramebuffers, frameSlot);
            RetireSafeFrameSlot(ColorFramebuffers, frameSlot);
            RetireSafeFrameSlot(BindingSets, frameSlot);
        }

        [[nodiscard]] uint64_t BeginGraphGeneration() noexcept
        {
            ++GraphGeneration;
            if (GraphGeneration == 0)
                GraphGeneration = 1;
            return GraphGeneration;
        }

        template<typename CacheType>
        [[nodiscard]] size_t CountStaleEntries(const CacheType& cache) const noexcept
        {
            return static_cast<size_t>(std::count_if(
                cache.begin(),
                cache.end(),
                [this](const auto& pair)
                {
                    return pair.second.LastGraphGeneration != GraphGeneration;
                }));
        }

        void ClearResourceCaches()
        {
            BindingSets.clear();
            UiFramebuffers.clear();
            ColorFramebuffers.clear();
            ++CacheGeneration;
            if (CacheGeneration == 0)
                CacheGeneration = 1;
        }

        nvrhi::DeviceHandle Device;
        ShaderManager ShaderManagerInstance;
        ShaderArtifactResolver ShaderArtifacts;
        PipelineCache PipelineCacheInstance;
        nvrhi::ShaderHandle VertexShader;
        nvrhi::ShaderHandle PixelShader;
        nvrhi::BindingLayoutHandle BindingLayout;
        nvrhi::SamplerHandle Sampler;
        RuntimeUiRenderFrameState Frame;
        uint64_t CacheGeneration = 1;
        uint64_t GraphGeneration = 0;
        std::unordered_map<UiFramebufferKey, UiFramebufferCacheEntry, UiFramebufferKeyHash>
            UiFramebuffers;
        std::unordered_map<nvrhi::ITexture*, UiFramebufferCacheEntry> ColorFramebuffers;
        std::unordered_map<nvrhi::ITexture*, UiBindingSetCacheEntry> BindingSets;
    };

    RuntimeUiRenderGraphPass::RuntimeUiRenderGraphPass(
        const RuntimeUiRenderGraphPassDesc& desc)
        : mImpl(std::make_unique<Impl>(desc))
    {
    }

    RuntimeUiRenderGraphPass::~RuntimeUiRenderGraphPass() = default;

    void RuntimeUiRenderGraphPass::UpdateFrame(const RuntimeUiRenderFrameState& frame)
    {
        mImpl->RetireSafeFrameSlot(frame.FrameSlot);
        mImpl->Frame = frame;
    }

    RuntimeUiRenderGraphResult RuntimeUiRenderGraphPass::AddToGraph(
        RenderGraph& graph,
        const RuntimeUiRenderGraphInputs& inputs)
    {
        if (!inputs.BackBuffer)
        {
            throw RenderGraphValidationError(
                "Runtime UI render graph pass requires a valid BackBuffer handle.");
        }
        if (inputs.Width == 0 || inputs.Height == 0)
        {
            throw RenderGraphValidationError(
                "Runtime UI render graph pass requires non-zero render dimensions.");
        }

        RuntimeUiRenderGraphResult result;
        Impl* const impl = mImpl.get();
        const uint64_t graphGeneration = impl->BeginGraphGeneration();
        result.RuntimeUiRenderPass = graph.AddPass<RuntimeUiRenderPassData>(
            "RuntimeUiRender",
            RenderGraphQueue::Graphics,
            [&result, inputs](RenderGraphBuilder& builder, RuntimeUiRenderPassData& data)
            {
                if (inputs.Backdrop)
                    data.Backdrop = builder.ShaderResource(inputs.Backdrop);
                data.UiColor = builder.CreateTexture(
                    MakeUiTextureDesc(
                        "Runtime UI 2x MSAA Premultiplied LDR Color",
                        inputs.Width,
                        inputs.Height,
                        c_UiColorFormat,
                        c_UiSampleCount),
                    RenderGraphResourceLifetime::Persistent);
                data.UiColor = builder.RenderTarget(data.UiColor);
                data.UiStencil = builder.CreateTexture(
                    MakeUiTextureDesc(
                        "Runtime UI 2x MSAA Clip Stencil",
                        inputs.Width,
                        inputs.Height,
                        c_UiStencilFormat,
                        c_UiSampleCount),
                    RenderGraphResourceLifetime::Persistent);
                data.UiStencil = builder.DepthWrite(data.UiStencil);
                result.UiColor = data.UiColor;
                result.UiStencil = data.UiStencil;
            },
            [impl, width = inputs.Width, height = inputs.Height, graphGeneration](
                RenderGraphContext& context,
                const RuntimeUiRenderPassData& data)
            {
                nvrhi::ITexture* backdrop = data.Backdrop
                    ? context.GetTexture(data.Backdrop, RenderGraphAccess::Read)
                    : nullptr;
                nvrhi::ITexture* color = context.GetTexture(
                    data.UiColor,
                    RenderGraphAccess::Write);
                nvrhi::ITexture* stencil = context.GetTexture(
                    data.UiStencil,
                    RenderGraphAccess::Write);
                nvrhi::ICommandList* commandList = context.GetCommandList();
                nvrhi::IFramebuffer* framebuffer =
                    impl->GetOrCreateUiFramebuffer(
                        color,
                        stencil,
                        graphGeneration,
                        impl->Frame.FrameSlot);

                commandList->clearTextureFloat(
                    color,
                    nvrhi::AllSubresources,
                    nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
                commandList->clearDepthStencilTexture(
                    stencil,
                    nvrhi::AllSubresources,
                    true,
                    1.0f,
                    true,
                    0);

                const RuntimeUiRenderRecorder recorder = impl->Frame.Recorder;
                if (recorder)
                {
                    recorder(RuntimeUiRenderContext{
                        .CommandList = commandList,
                        .Framebuffer = framebuffer,
                        .ColorTarget = color,
                        .StencilTarget = stencil,
                        .BackdropTexture = backdrop,
                        .Width = width,
                        .Height = height,
                        .FrameSlot = impl->Frame.FrameSlot,
                        .CompletedSubmission = impl->Frame.CompletedSubmission,
                    });
                }
            });

        result.UiColor = AddTextureResolvePass(
            graph,
            result.UiColor,
            "Runtime UI Resolved Premultiplied LDR Color");

        const TextureHandle uiColor = result.UiColor;
        result.UiTransitionPass = graph.AddPass<UiTransitionPassData>(
            "UiTransition",
            RenderGraphQueue::Graphics,
            [&result, inputs, uiColor](RenderGraphBuilder& builder, UiTransitionPassData& data)
            {
                data.UiColor = builder.ShaderResource(uiColor);
                data.TransitionColor = builder.CreateTexture(
                    MakeUiTextureDesc(
                        "Runtime UI Transition Color",
                        inputs.Width,
                        inputs.Height,
                        c_UiColorFormat),
                    RenderGraphResourceLifetime::Persistent);
                data.TransitionColor = builder.RenderTarget(data.TransitionColor);
                result.TransitionColor = data.TransitionColor;
            },
            [impl, width = inputs.Width, height = inputs.Height, graphGeneration](
                RenderGraphContext& context,
                const UiTransitionPassData& data)
            {
                nvrhi::ITexture* source = context.GetTexture(
                    data.UiColor,
                    RenderGraphAccess::Read);
                nvrhi::ITexture* destination = context.GetTexture(
                    data.TransitionColor,
                    RenderGraphAccess::Write);
                impl->RecordFullscreen(
                    context,
                    source,
                    destination,
                    width,
                    height,
                    false,
                    graphGeneration);
            });

        const TextureHandle transitionColor = result.TransitionColor;
        result.UiCompositePass = graph.AddPass<UiCompositePassData>(
            "UiComposite",
            RenderGraphQueue::Graphics,
            [&result, inputs, transitionColor](
                RenderGraphBuilder& builder,
                UiCompositePassData& data)
            {
                data.TransitionColor = builder.ShaderResource(transitionColor);
                data.BackBuffer = builder.ReadWrite(
                    inputs.BackBuffer,
                    RenderGraphUsage::RenderTarget);
                result.CompositedBackBuffer = data.BackBuffer;
            },
            [impl, width = inputs.Width, height = inputs.Height, graphGeneration](
                RenderGraphContext& context,
                const UiCompositePassData& data)
            {
                nvrhi::ITexture* source = context.GetTexture(
                    data.TransitionColor,
                    RenderGraphAccess::Read);
                nvrhi::ITexture* destination = context.GetTexture(
                    data.BackBuffer,
                    RenderGraphAccess::ReadWrite);
                impl->RecordFullscreen(
                    context,
                    source,
                    destination,
                    width,
                    height,
                    true,
                    graphGeneration);
            });

        result.bAdded = true;
        return result;
    }

    void RuntimeUiRenderGraphPass::ClearResourceCaches()
    {
        mImpl->ClearResourceCaches();
    }

    std::function<void()> RuntimeUiRenderGraphPass::CaptureGraphState()
    {
        return [impl = mImpl.get(), uiFramebuffers = mImpl->UiFramebuffers,
            colorFramebuffers = mImpl->ColorFramebuffers, bindings = mImpl->BindingSets,
            cacheGeneration = mImpl->CacheGeneration,
            graphGeneration = mImpl->GraphGeneration]() mutable noexcept
        {
            impl->UiFramebuffers.swap(uiFramebuffers);
            impl->ColorFramebuffers.swap(colorFramebuffers);
            impl->BindingSets.swap(bindings);
            std::swap(impl->CacheGeneration, cacheGeneration);
            std::swap(impl->GraphGeneration, graphGeneration);
        };
    }

    void RuntimeUiRenderGraphPass::ClearGraphResourceCaches() noexcept
    {
        mImpl->UiFramebuffers.clear();
        mImpl->ColorFramebuffers.clear();
        mImpl->BindingSets.clear();
    }

    size_t RuntimeUiRenderGraphPass::GetFramebufferCacheSize() const noexcept
    {
        return mImpl->UiFramebuffers.size() + mImpl->ColorFramebuffers.size();
    }

    size_t RuntimeUiRenderGraphPass::GetBindingSetCacheSize() const noexcept
    {
        return mImpl->BindingSets.size();
    }

    size_t RuntimeUiRenderGraphPass::GetStaleFramebufferCacheSize() const noexcept
    {
        return mImpl->CountStaleEntries(mImpl->UiFramebuffers) +
            mImpl->CountStaleEntries(mImpl->ColorFramebuffers);
    }

    size_t RuntimeUiRenderGraphPass::GetStaleBindingSetCacheSize() const noexcept
    {
        return mImpl->CountStaleEntries(mImpl->BindingSets);
    }

    uint64_t RuntimeUiRenderGraphPass::GetCacheGeneration() const noexcept
    {
        return mImpl->CacheGeneration;
    }

    uint64_t RuntimeUiRenderGraphPass::GetGraphGeneration() const noexcept
    {
        return mImpl->GraphGeneration;
    }
}
