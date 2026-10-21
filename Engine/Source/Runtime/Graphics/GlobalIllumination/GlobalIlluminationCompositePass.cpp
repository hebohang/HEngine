#include "Hepch.h"

#include "Runtime/Graphics/GlobalIllumination/GlobalIlluminationCompositePass.h"

#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <glm/glm.hpp>

#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace HEngine
{
    namespace
    {
        struct alignas(16) CompositeConstants
        {
            glm::uvec2 OutputDimensions{ 0u };
            uint32_t ScalarInput = 0;
            uint32_t SampleCount = 1;
        };

        static_assert(sizeof(CompositeConstants) == 16);

        struct CompositePassData
        {
            TextureHandle SceneColor;
            TextureHandle GlobalIllumination;
            TextureHandle MultisampleDepth;
            TextureHandle RepresentativeDepth;
            GlobalIlluminationCompositeMode Mode =
                GlobalIlluminationCompositeMode::Additive;
            bool bMultisample = false;
        };

        struct MultisampleBindingKey
        {
            nvrhi::ITexture* GlobalIllumination = nullptr;
            nvrhi::ITexture* Depth = nullptr;
            nvrhi::ITexture* RepresentativeDepth = nullptr;

            [[nodiscard]] bool operator==(
                const MultisampleBindingKey&) const = default;
        };

        struct MultisampleBindingKeyHash
        {
            [[nodiscard]] size_t operator()(
                const MultisampleBindingKey& key) const noexcept
            {
                size_t hash = reinterpret_cast<size_t>(
                    key.GlobalIllumination);
                hash ^= reinterpret_cast<size_t>(key.Depth) +
                    0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                hash ^= reinterpret_cast<size_t>(key.RepresentativeDepth) +
                    0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                return hash;
            }
        };

        struct CompositePipelineKey
        {
            nvrhi::IFramebuffer* Framebuffer = nullptr;
            GlobalIlluminationCompositeMode Mode =
                GlobalIlluminationCompositeMode::Additive;
            bool bMultisample = false;

            [[nodiscard]] bool operator==(
                const CompositePipelineKey&) const = default;
        };

        struct CompositePipelineKeyHash
        {
            [[nodiscard]] size_t operator()(
                const CompositePipelineKey& key) const noexcept
            {
                size_t hash = reinterpret_cast<size_t>(key.Framebuffer);
                hash ^= static_cast<size_t>(key.Mode) + 0x9e3779b9u +
                    (hash << 6u) + (hash >> 2u);
                hash ^= static_cast<size_t>(key.bMultisample) +
                    0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                return hash;
            }
        };

        [[nodiscard]] std::string ReadShaderContentHash(
            const std::filesystem::path& path)
        {
            std::ifstream stream(path);
            if (!stream)
            {
                throw std::runtime_error(
                    "Failed to open GI composite shader cache-key sidecar: " +
                    Utils::PathToUtf8(path));
            }
            std::string line;
            while (std::getline(stream, line))
            {
                constexpr std::string_view Prefix = "key=";
                if (line.starts_with(Prefix) && line.size() > Prefix.size())
                    return line.substr(Prefix.size());
            }
            throw std::runtime_error("GI composite shader cache-key sidecar has no key");
        }
    }

    struct GlobalIlluminationCompositePass::Impl
    {
        explicit Impl(const GlobalIlluminationCompositePassDesc& desc)
            : Device(desc.Device)
            , OwnedShaderManager(desc.SharedShaderManager
                ? nullptr
                : CreateScope<ShaderManager>(desc.Device))
            , ShaderManagerInstance(desc.SharedShaderManager
                ? *desc.SharedShaderManager
                : *OwnedShaderManager)
            , ShaderArtifactCacheRoot(desc.ShaderArtifactCacheRoot)
            , Backend(desc.Backend)
            , BinaryFormat(GetShaderBinaryFormat(desc.Backend))
        {
            if (!Device)
                throw std::invalid_argument("GI composite pass requires a valid NVRHI device");
            if (ShaderArtifactCacheRoot.empty())
                throw std::invalid_argument("GI composite pass requires a shader artifact cache root");
            CreateImmutableState();
        }

        void CreateImmutableState()
        {
            const std::filesystem::path artifactDirectory =
                ShaderArtifactCacheRoot /
                std::string(GetShaderArtifactDirectoryName(BinaryFormat));
            const std::filesystem::path vertexPath = artifactDirectory /
                "GlobalIlluminationComposite.vs";
            const std::filesystem::path pixelPath = artifactDirectory /
                "GlobalIlluminationComposite.ps";
            const std::filesystem::path multisamplePixelPath = artifactDirectory /
                "GlobalIlluminationComposite_MULTISAMPLE.ps";
            VertexShader = ShaderManagerInstance.LoadShaderFile(
                ShaderCacheKey{
                    .Source = "GlobalIllumination/GlobalIlluminationComposite.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Vertex,
                    .EntryPoint = "VS",
                    .TargetProfile = "vs_6_0",
                    .ContentHash = ReadShaderContentHash(
                        MakeShaderCacheKeyPath(vertexPath)),
                    .Backend = Backend,
                    .BinaryFormat = BinaryFormat,
                },
                vertexPath,
                "GI Composite VS");
            PixelShader = ShaderManagerInstance.LoadShaderFile(
                ShaderCacheKey{
                    .Source = "GlobalIllumination/GlobalIlluminationComposite.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = "ps_6_0",
                    .ContentHash = ReadShaderContentHash(
                        MakeShaderCacheKeyPath(pixelPath)),
                    .Backend = Backend,
                    .BinaryFormat = BinaryFormat,
                },
                pixelPath,
                "GI Composite PS");
            MultisamplePixelShader = ShaderManagerInstance.LoadShaderFile(
                ShaderCacheKey{
                    .Source = "GlobalIllumination/GlobalIlluminationComposite.hlsl",
                    .Variant = "MULTISAMPLE",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = "ps_6_0",
                    .ContentHash = ReadShaderContentHash(
                        MakeShaderCacheKeyPath(multisamplePixelPath)),
                    .Backend = Backend,
                    .BinaryFormat = BinaryFormat,
                },
                multisamplePixelPath,
                "GI Multisample Composite PS");
            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.setVisibility(nvrhi::ShaderType::Pixel)
                .setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::PushConstants(
                    0,
                    sizeof(CompositeConstants)));
            BindingLayout = Device->createBindingLayout(layoutDesc);
            nvrhi::BindingLayoutDesc multisampleLayoutDesc;
            multisampleLayoutDesc.setVisibility(nvrhi::ShaderType::Pixel)
                .setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(2))
                .addItem(nvrhi::BindingLayoutItem::PushConstants(
                    0,
                    sizeof(CompositeConstants)));
            MultisampleBindingLayout = Device->createBindingLayout(
                multisampleLayoutDesc);
            if (!VertexShader || !PixelShader || !MultisamplePixelShader ||
                !BindingLayout || !MultisampleBindingLayout)
                throw std::runtime_error("NVRHI failed to create GI composite immutable state");
        }

        [[nodiscard]] nvrhi::IBindingSet* GetBindingSet(
            nvrhi::ITexture* globalIllumination)
        {
            const auto existing = BindingSets.find(globalIllumination);
            if (existing != BindingSets.end())
                return existing->second;
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::Texture_SRV(
                0,
                globalIllumination))
                .addItem(nvrhi::BindingSetItem::PushConstants(
                    0,
                    sizeof(CompositeConstants)));
            nvrhi::BindingSetHandle bindingSet = Device->createBindingSet(
                desc,
                BindingLayout);
            if (!bindingSet)
                throw std::runtime_error("NVRHI failed to bind a GI composite input");
            return BindingSets.emplace(
                globalIllumination,
                std::move(bindingSet)).first->second;
        }

        [[nodiscard]] nvrhi::IFramebuffer* GetFramebuffer(nvrhi::ITexture* sceneColor)
        {
            const auto existing = Framebuffers.find(sceneColor);
            if (existing != Framebuffers.end())
                return existing->second;
            nvrhi::FramebufferDesc desc;
            desc.addColorAttachment(sceneColor);
            nvrhi::FramebufferHandle framebuffer = Device->createFramebuffer(desc);
            if (!framebuffer)
                throw std::runtime_error("NVRHI failed to create a GI composite framebuffer");
            return Framebuffers.emplace(
                sceneColor,
                std::move(framebuffer)).first->second;
        }

        [[nodiscard]] nvrhi::IBindingSet* GetMultisampleBindingSet(
            nvrhi::ITexture* globalIllumination,
            nvrhi::ITexture* depth,
            nvrhi::ITexture* representativeDepth)
        {
            const MultisampleBindingKey key{
                globalIllumination,
                depth,
                representativeDepth,
            };
            const auto existing = MultisampleBindingSets.find(key);
            if (existing != MultisampleBindingSets.end())
                return existing->second;
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::Texture_SRV(
                0,
                globalIllumination))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(1, depth))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(
                    2,
                    representativeDepth))
                .addItem(nvrhi::BindingSetItem::PushConstants(
                    0,
                    sizeof(CompositeConstants)));
            nvrhi::BindingSetHandle bindingSet = Device->createBindingSet(
                desc,
                MultisampleBindingLayout);
            if (!bindingSet)
            {
                throw std::runtime_error(
                    "NVRHI failed to bind multisample GI composite inputs");
            }
            return MultisampleBindingSets.emplace(
                key,
                std::move(bindingSet)).first->second;
        }

        [[nodiscard]] nvrhi::IGraphicsPipeline* GetPipeline(
            nvrhi::IFramebuffer* framebuffer,
            GlobalIlluminationCompositeMode mode,
            bool bMultisample)
        {
            const CompositePipelineKey key{ framebuffer, mode, bMultisample };
            const auto existing = Pipelines.find(key);
            if (existing != Pipelines.end())
                return existing->second;
            if (bMultisample &&
                mode != GlobalIlluminationCompositeMode::Additive)
            {
                throw std::invalid_argument(
                    "Multisample GI composite supports additive mode only");
            }
            nvrhi::GraphicsPipelineDesc desc;
            desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setVertexShader(VertexShader)
                .setPixelShader(bMultisample
                    ? MultisamplePixelShader
                    : PixelShader)
                .addBindingLayout(bMultisample
                    ? MultisampleBindingLayout
                    : BindingLayout);
            desc.renderState.depthStencilState
                .disableDepthTest()
                .disableDepthWrite();
            desc.renderState.rasterState.setCullNone();
            if (framebuffer->getFramebufferInfo().sampleCount > 1)
                desc.renderState.rasterState.enableMultisample();
            if (mode == GlobalIlluminationCompositeMode::Additive)
            {
                desc.renderState.blendState.targets[0]
                    .enableBlend()
                    .setSrcBlend(nvrhi::BlendFactor::One)
                    .setDestBlend(nvrhi::BlendFactor::One)
                    .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                    .setDestBlendAlpha(nvrhi::BlendFactor::Zero);
            }
            nvrhi::GraphicsPipelineHandle pipeline =
                Device->createGraphicsPipeline(
                desc,
                framebuffer->getFramebufferInfo());
            if (!pipeline)
                throw std::runtime_error("NVRHI failed to create a GI composite pipeline");
            return Pipelines.emplace(
                key,
                std::move(pipeline)).first->second;
        }

        nvrhi::DeviceHandle Device;
        Scope<ShaderManager> OwnedShaderManager;
        ShaderManager& ShaderManagerInstance;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
        ShaderBinaryFormat BinaryFormat = ShaderBinaryFormat::Dxil;
        nvrhi::ShaderHandle VertexShader;
        nvrhi::ShaderHandle PixelShader;
        nvrhi::ShaderHandle MultisamplePixelShader;
        nvrhi::BindingLayoutHandle BindingLayout;
        nvrhi::BindingLayoutHandle MultisampleBindingLayout;
        std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> BindingSets;
        std::unordered_map<
            MultisampleBindingKey,
            nvrhi::BindingSetHandle,
            MultisampleBindingKeyHash> MultisampleBindingSets;
        std::unordered_map<nvrhi::ITexture*, nvrhi::FramebufferHandle> Framebuffers;
        std::unordered_map<
            CompositePipelineKey,
            nvrhi::GraphicsPipelineHandle,
            CompositePipelineKeyHash> Pipelines;
        CompositeConstants Constants;
        uint32_t Width = 0;
        uint32_t Height = 0;
    };

    GlobalIlluminationCompositePass::GlobalIlluminationCompositePass(
        const GlobalIlluminationCompositePassDesc& desc)
        : mImpl(CreateScope<Impl>(desc))
    {
    }

    GlobalIlluminationCompositePass::~GlobalIlluminationCompositePass() = default;

    TextureHandle GlobalIlluminationCompositePass::AddPass(
        RenderGraph& renderGraph,
        TextureHandle sceneColor,
        TextureHandle globalIllumination,
        GlobalIlluminationCompositeMode mode,
        uint32_t width,
        uint32_t height,
        uint32_t inputChannel)
    {
        if (!sceneColor || !globalIllumination || width == 0 || height == 0)
            throw std::invalid_argument("GI composite pass inputs are incomplete");
        mImpl->Width = width;
        mImpl->Height = height;
        mImpl->Constants.OutputDimensions = glm::uvec2(width, height);
        mImpl->Constants.ScalarInput = inputChannel;
        mImpl->Constants.SampleCount = 1u;
        renderGraph.AddPass<CompositePassData>(
            mode == GlobalIlluminationCompositeMode::Additive
                ? "GIComposite"
                : "OfflineViewportComposite",
            RenderGraphQueue::Graphics,
            [sceneColor, globalIllumination, mode](
                RenderGraphBuilder& builder,
                CompositePassData& data)
            {
                data.SceneColor = mode == GlobalIlluminationCompositeMode::Additive
                    ? builder.ReadWrite(
                        sceneColor,
                        RenderGraphUsage::RenderTarget)
                    : builder.Write(
                        sceneColor,
                        RenderGraphUsage::RenderTarget);
                data.GlobalIllumination = builder.ShaderResource(globalIllumination);
                data.Mode = mode;
            },
            [impl = mImpl.get()](
                RenderGraphContext& context,
                const CompositePassData& data)
            {
                nvrhi::ITexture* sceneColor = context.GetTexture(
                    data.SceneColor,
                    data.Mode == GlobalIlluminationCompositeMode::Additive
                        ? RenderGraphAccess::ReadWrite
                        : RenderGraphAccess::Write);
                nvrhi::ITexture* globalIllumination = context.GetTexture(
                    data.GlobalIllumination,
                    RenderGraphAccess::Read);
                nvrhi::IFramebuffer* framebuffer = impl->GetFramebuffer(sceneColor);
                nvrhi::GraphicsState state;
                state.setPipeline(impl->GetPipeline(
                        framebuffer,
                        data.Mode,
                        false))
                    .setFramebuffer(framebuffer)
                    .setViewport(nvrhi::ViewportState()
                        .addViewportAndScissorRect(nvrhi::Viewport(
                            static_cast<float>(impl->Width),
                            static_cast<float>(impl->Height))))
                    .addBindingSet(impl->GetBindingSet(globalIllumination));
                context.GetCommandList()->setGraphicsState(state);
                context.GetCommandList()->setPushConstants(
                    &impl->Constants,
                    sizeof(impl->Constants));
                context.GetCommandList()->draw(
                    nvrhi::DrawArguments().setVertexCount(3));
            });
        return sceneColor;
    }

    TextureHandle GlobalIlluminationCompositePass::AddMultisamplePass(
        RenderGraph& renderGraph,
        TextureHandle multisampleSceneColor,
        TextureHandle multisampleSceneDepth,
        TextureHandle representativeSceneDepth,
        TextureHandle globalIllumination,
        uint32_t width,
        uint32_t height,
        uint32_t sampleCount,
        uint32_t inputChannel)
    {
        if (!multisampleSceneColor || !multisampleSceneDepth ||
            !representativeSceneDepth || !globalIllumination ||
            width == 0 || height == 0)
        {
            throw std::invalid_argument(
                "Multisample GI composite inputs are incomplete");
        }
        if (sampleCount != 2 && sampleCount != 4 && sampleCount != 8)
        {
            throw std::invalid_argument(
                "Multisample GI composite sample count must be 2, 4, or 8");
        }
        const RenderGraphTextureDesc& colorDesc =
            renderGraph.GetTextureDesc(multisampleSceneColor);
        const RenderGraphTextureDesc& depthDesc =
            renderGraph.GetTextureDesc(multisampleSceneDepth);
        const RenderGraphTextureDesc& giDesc =
            renderGraph.GetTextureDesc(globalIllumination);
        const RenderGraphTextureDesc& representativeDepthDesc =
            renderGraph.GetTextureDesc(representativeSceneDepth);
        if (colorDesc.SampleCount != sampleCount ||
            depthDesc.SampleCount != sampleCount || giDesc.SampleCount != 1 ||
            representativeDepthDesc.SampleCount != 1 ||
            nvrhi::getFormatInfo(colorDesc.Format).hasDepth ||
            !nvrhi::getFormatInfo(depthDesc.Format).hasDepth ||
            !nvrhi::getFormatInfo(representativeDepthDesc.Format).hasDepth)
        {
            throw std::invalid_argument(
                "Multisample GI composite texture descriptions are incompatible");
        }

        mImpl->Width = width;
        mImpl->Height = height;
        mImpl->Constants.OutputDimensions = glm::uvec2(width, height);
        mImpl->Constants.ScalarInput = inputChannel;
        mImpl->Constants.SampleCount = sampleCount;
        renderGraph.AddPass<CompositePassData>(
            "GICompositeMSAA",
            RenderGraphQueue::Graphics,
            [multisampleSceneColor,
                multisampleSceneDepth,
                representativeSceneDepth,
                globalIllumination](
                RenderGraphBuilder& builder,
                CompositePassData& data)
            {
                data.SceneColor = builder.ReadWrite(
                    multisampleSceneColor,
                    RenderGraphUsage::RenderTarget);
                data.MultisampleDepth = builder.ShaderResource(
                    multisampleSceneDepth);
                data.RepresentativeDepth = builder.ShaderResource(
                    representativeSceneDepth);
                data.GlobalIllumination = builder.ShaderResource(
                    globalIllumination);
                data.Mode = GlobalIlluminationCompositeMode::Additive;
                data.bMultisample = true;
            },
            [impl = mImpl.get()](
                RenderGraphContext& context,
                const CompositePassData& data)
            {
                nvrhi::ITexture* sceneColor = context.GetTexture(
                    data.SceneColor,
                    RenderGraphAccess::ReadWrite);
                nvrhi::ITexture* globalIllumination = context.GetTexture(
                    data.GlobalIllumination,
                    RenderGraphAccess::Read);
                nvrhi::ITexture* depth = context.GetTexture(
                    data.MultisampleDepth,
                    RenderGraphAccess::Read);
                nvrhi::ITexture* representativeDepth = context.GetTexture(
                    data.RepresentativeDepth,
                    RenderGraphAccess::Read);
                nvrhi::IFramebuffer* framebuffer = impl->GetFramebuffer(
                    sceneColor);
                nvrhi::GraphicsState state;
                state.setPipeline(impl->GetPipeline(
                        framebuffer,
                        GlobalIlluminationCompositeMode::Additive,
                        true))
                    .setFramebuffer(framebuffer)
                    .setViewport(nvrhi::ViewportState()
                        .addViewportAndScissorRect(nvrhi::Viewport(
                            static_cast<float>(impl->Width),
                            static_cast<float>(impl->Height))))
                    .addBindingSet(impl->GetMultisampleBindingSet(
                        globalIllumination,
                        depth,
                        representativeDepth));
                nvrhi::ICommandList* commandList = context.GetCommandList();
                commandList->setGraphicsState(state);
                commandList->setPushConstants(
                    &impl->Constants,
                    sizeof(impl->Constants));
                commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
            });
        return multisampleSceneColor;
    }

    void GlobalIlluminationCompositePass::ClearCaches()
    {
        mImpl->BindingSets.clear();
        mImpl->MultisampleBindingSets.clear();
        mImpl->Framebuffers.clear();
        mImpl->Pipelines.clear();
    }

    std::function<void()> GlobalIlluminationCompositePass::CaptureGraphState()
    {
        return [impl = mImpl.get(), bindings = mImpl->BindingSets,
            multisample = mImpl->MultisampleBindingSets, frames = mImpl->Framebuffers,
            constants = mImpl->Constants, width = mImpl->Width, height = mImpl->Height]() mutable noexcept
        {
            using std::swap;
            swap(impl->BindingSets, bindings); swap(impl->MultisampleBindingSets, multisample);
            swap(impl->Framebuffers, frames); swap(impl->Constants, constants);
            swap(impl->Width, width); swap(impl->Height, height);
        };
    }

    void GlobalIlluminationCompositePass::ClearGraphResourceCaches() noexcept
    {
        mImpl->BindingSets.clear(); mImpl->MultisampleBindingSets.clear(); mImpl->Framebuffers.clear();
    }
}
