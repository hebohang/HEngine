#include "Hepch.h"

#include "Runtime/Graphics/Renderer/SceneAntiAliasingPass.h"

#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace HEngine
{
    namespace
    {
        constexpr uint32_t c_TaaConvergenceFrameCount = 16u;

        struct alignas(16) FxaaConstants
        {
            glm::uvec2 Dimensions{ 0u };
            float EdgeThreshold = 0.125f;
            float EdgeThresholdMin = 0.0312f;
            float SubpixelQuality = 0.75f;
            glm::vec3 Padding{ 0.0f };
        };

        struct alignas(16) TaaConstants
        {
            glm::uvec2 Dimensions{ 0u };
            float HistoryWeight = 0.9f;
            float Sharpness = 0.2f;
            uint32_t ResetHistory = 1;
            float ViewDepthThreshold = 0.01f;
            float NormalRejectionCosine = 0.9f;
            float MotionRejectionPixels = 64.0f;
            uint32_t HasTemporalGiConfidence = 0;
            float LuminanceReactiveScale = 4.0f;
            float TemporalGiHistoryCeiling = 0.90f;
            float Padding = 0.0f;
        };

        struct alignas(16) SmaaConstants
        {
            glm::uvec2 Dimensions{ 0u };
            float EdgeThreshold = 0.10f;
            float LocalContrastAdaptation = 2.0f;
            uint32_t MaxSearchSteps = 24;
            uint32_t MaxDiagonalSearchSteps = 8;
            float CornerRounding = 0.25f;
            float Padding = 0.0f;
        };

        static_assert(sizeof(FxaaConstants) == 32);
        static_assert(sizeof(TaaConstants) == 48);
        static_assert(sizeof(SmaaConstants) == 32);

        enum class SmaaStage : uint8_t
        {
            EdgeDetection,
            BlendWeight,
            Neighborhood,
            Count,
        };

        struct FxaaPassData
        {
            TextureHandle Source;
            TextureHandle Target;
        };

        struct SmaaPassData
        {
            TextureHandle Source0;
            TextureHandle Source1;
            TextureHandle Target;
            SmaaStage Stage = SmaaStage::EdgeDetection;
        };

        struct TaaHistoryHandles
        {
            TextureHandle Color;
            TextureHandle ViewDepth;
            TextureHandle NormalLength;
            TextureHandle SurfaceId;
        };

        struct TaaPassData
        {
            SceneTaaPassInputs Inputs;
            TaaHistoryHandles Previous;
            TaaHistoryHandles Current;
            TextureHandle Resolved;
            bool bHasTemporalGiConfidence = false;
        };

        struct TaaBindingKey
        {
            std::array<nvrhi::ITexture*, 16> Textures{};

            [[nodiscard]] bool operator==(const TaaBindingKey&) const = default;
        };

        struct TaaBindingKeyHash
        {
            [[nodiscard]] size_t operator()(const TaaBindingKey& key) const noexcept
            {
                size_t hash = 0x544141u;
                for (nvrhi::ITexture* texture : key.Textures)
                {
                    hash ^= reinterpret_cast<size_t>(texture) + 0x9e3779b9u +
                        (hash << 6u) + (hash >> 2u);
                }
                return hash;
            }
        };

        struct SmaaBindingKey
        {
            std::array<nvrhi::ITexture*, 2> Textures{};

            [[nodiscard]] bool operator==(const SmaaBindingKey&) const = default;
        };

        struct SmaaBindingKeyHash
        {
            [[nodiscard]] size_t operator()(
                const SmaaBindingKey& key) const noexcept
            {
                size_t hash = 0x534d4141u;
                for (nvrhi::ITexture* texture : key.Textures)
                {
                    hash ^= reinterpret_cast<size_t>(texture) + 0x9e3779b9u +
                        (hash << 6u) + (hash >> 2u);
                }
                return hash;
            }
        };

        [[nodiscard]] constexpr size_t ToIndex(SmaaStage stage) noexcept
        {
            return static_cast<size_t>(stage);
        }

        [[nodiscard]] uint32_t DispatchCount(uint32_t value) noexcept
        {
            return (value + 7u) / 8u;
        }

        [[nodiscard]] std::string ReadShaderContentHash(
            const std::filesystem::path& path)
        {
            std::ifstream stream(path);
            if (!stream)
            {
                throw std::runtime_error(
                    "Failed to open scene AA shader cache-key sidecar: " +
                    Utils::PathToUtf8(path));
            }
            std::string line;
            while (std::getline(stream, line))
            {
                constexpr std::string_view Prefix = "key=";
                if (line.starts_with(Prefix) && line.size() > Prefix.size())
                    return line.substr(Prefix.size());
            }
            throw std::runtime_error(
                "Scene AA shader cache-key sidecar has no key: " +
                Utils::PathToUtf8(path));
        }

        [[nodiscard]] const RenderGraphTextureDesc& RequireSingleSampleTexture(
            const RenderGraph& renderGraph,
            TextureHandle handle,
            std::string_view name)
        {
            if (!handle || !renderGraph.IsValid(handle))
            {
                throw RenderGraphValidationError(
                    "Scene AA requires a valid " + std::string(name) +
                    " texture handle");
            }
            const RenderGraphTextureDesc& desc = renderGraph.GetTextureDesc(handle);
            if (desc.SampleCount != 1 ||
                desc.Dimension != nvrhi::TextureDimension::Texture2D)
            {
                throw RenderGraphValidationError(
                    "Scene AA requires single-sample Texture2D input for " +
                    std::string(name));
            }
            return desc;
        }

        void RequireFormat(
            const RenderGraphTextureDesc& desc,
            nvrhi::Format expected,
            std::string_view name)
        {
            if (desc.Format != expected)
            {
                throw RenderGraphValidationError(
                    "Scene AA texture '" + std::string(name) +
                    "' has an incompatible format");
            }
        }
    }

    struct SceneAntiAliasingPass::Impl
    {
        struct HistorySet
        {
            nvrhi::TextureHandle Color;
            nvrhi::TextureHandle ViewDepth;
            nvrhi::TextureHandle NormalLength;
            nvrhi::TextureHandle SurfaceId;
        };

        struct RetiredHistory
        {
            uint64_t Submission = 0;
            std::array<HistorySet, 2> Sets;
        };

        struct RetiredResources
        {
            uint64_t Submission = 0;
            std::vector<nvrhi::BindingSetHandle> BindingSets;
            std::vector<nvrhi::FramebufferHandle> Framebuffers;
        };

        explicit Impl(const SceneAntiAliasingPassDesc& desc)
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
            {
                throw std::invalid_argument(
                    "SceneAntiAliasingPass requires a valid NVRHI device");
            }
            if (ShaderArtifactCacheRoot.empty())
            {
                throw std::invalid_argument(
                    "SceneAntiAliasingPass requires a shader artifact cache root");
            }
            CreateImmutableState();
        }

        [[nodiscard]] nvrhi::ShaderHandle LoadShader(
            std::string_view source,
            std::string_view artifactName,
            nvrhi::ShaderType stage,
            std::string_view entryPoint,
            std::string_view targetProfile,
            std::string_view debugName,
            std::string_view variant = "BASE")
        {
            const std::filesystem::path path = ShaderArtifactCacheRoot /
                std::string(GetShaderArtifactDirectoryName(BinaryFormat)) /
                std::string(artifactName);
            return ShaderManagerInstance.LoadShaderFile(
                ShaderCacheKey{
                    .Source = std::string(source),
                    .Variant = std::string(variant),
                    .Stage = stage,
                    .EntryPoint = std::string(entryPoint),
                    .TargetProfile = std::string(targetProfile),
                    .ContentHash = ReadShaderContentHash(
                        MakeShaderCacheKeyPath(path)),
                    .Backend = Backend,
                    .BinaryFormat = BinaryFormat,
                },
                path,
                debugName);
        }

        void CreateImmutableState()
        {
            FxaaVertexShader = LoadShader(
                "Modern/SceneFxaa.hlsl",
                "SceneFxaa.vs",
                nvrhi::ShaderType::Vertex,
                "VS",
                "vs_6_0",
                "Scene FXAA fullscreen VS");
            FxaaPixelShader = LoadShader(
                "Modern/SceneFxaa.hlsl",
                "SceneFxaa.ps",
                nvrhi::ShaderType::Pixel,
                "PS",
                "ps_6_0",
                "Scene FXAA PS");
            TaaComputeShader = LoadShader(
                "Modern/SceneTaa.hlsl",
                "SceneTaa.cs",
                nvrhi::ShaderType::Compute,
                "CS",
                "cs_6_0",
                "Scene TAA CS");
            SmaaVertexShader = LoadShader(
                "Modern/SceneSmaa.hlsl",
                "SceneSmaa.vs",
                nvrhi::ShaderType::Vertex,
                "VS",
                "vs_6_0",
                "Scene SMAA fullscreen VS");
            SmaaPixelShaders[ToIndex(SmaaStage::EdgeDetection)] = LoadShader(
                "Modern/SceneSmaa.hlsl",
                "SceneSmaa.ps",
                nvrhi::ShaderType::Pixel,
                "PS",
                "ps_6_0",
                "Scene SMAA edge-detection PS");
            SmaaPixelShaders[ToIndex(SmaaStage::BlendWeight)] = LoadShader(
                "Modern/SceneSmaa.hlsl",
                "SceneSmaa_BLEND_WEIGHT.ps",
                nvrhi::ShaderType::Pixel,
                "PS",
                "ps_6_0",
                "Scene SMAA blend-weight PS",
                "BLEND_WEIGHT");
            SmaaPixelShaders[ToIndex(SmaaStage::Neighborhood)] = LoadShader(
                "Modern/SceneSmaa.hlsl",
                "SceneSmaa_NEIGHBORHOOD.ps",
                nvrhi::ShaderType::Pixel,
                "PS",
                "ps_6_0",
                "Scene SMAA neighborhood PS",
                "NEIGHBORHOOD");

            nvrhi::BindingLayoutDesc fxaaLayoutDesc;
            fxaaLayoutDesc.setVisibility(nvrhi::ShaderType::Pixel)
                .setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(
                    0,
                    sizeof(FxaaConstants)))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Sampler(0));
            FxaaBindingLayout = Device->createBindingLayout(fxaaLayoutDesc);

            nvrhi::BindingLayoutDesc smaaLayoutDesc;
            smaaLayoutDesc.setVisibility(nvrhi::ShaderType::Pixel)
                .setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(
                    0,
                    sizeof(SmaaConstants)))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::Sampler(0));
            SmaaBindingLayout = Device->createBindingLayout(smaaLayoutDesc);

            nvrhi::BindingLayoutDesc taaLayoutDesc;
            taaLayoutDesc.setVisibility(nvrhi::ShaderType::Compute)
                .setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(
                    0,
                    sizeof(TaaConstants)));
            for (uint32_t slot = 0; slot < 11; ++slot)
                taaLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot));
            for (uint32_t slot = 0; slot < 5; ++slot)
                taaLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(slot));
            TaaBindingLayout = Device->createBindingLayout(taaLayoutDesc);

            nvrhi::SamplerDesc samplerDesc;
            samplerDesc.setAllFilters(true)
                .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
            LinearClampSampler = Device->createSampler(samplerDesc);

            nvrhi::ComputePipelineDesc taaPipelineDesc;
            taaPipelineDesc.setComputeShader(TaaComputeShader)
                .addBindingLayout(TaaBindingLayout);
            TaaPipeline = Device->createComputePipeline(taaPipelineDesc);
            if (!FxaaVertexShader || !FxaaPixelShader || !TaaComputeShader ||
                !SmaaVertexShader ||
                std::ranges::any_of(
                    SmaaPixelShaders,
                    [](const nvrhi::ShaderHandle& shader)
                    {
                        return !shader;
                    }) ||
                !FxaaBindingLayout || !SmaaBindingLayout ||
                !TaaBindingLayout || !LinearClampSampler || !TaaPipeline)
            {
                throw std::runtime_error(
                    "NVRHI failed to create scene anti-aliasing immutable state");
            }
        }

        [[nodiscard]] nvrhi::TextureHandle CreateHistoryTexture(
            nvrhi::Format format,
            std::string debugName)
        {
            nvrhi::TextureDesc desc;
            desc.setWidth(Width)
                .setHeight(Height)
                .setFormat(format)
                .setIsUAV(true)
                .setInitialState(nvrhi::ResourceStates::ShaderResource)
                .setDebugName(std::move(debugName));
            nvrhi::TextureHandle texture = Device->createTexture(desc);
            if (!texture)
                throw std::runtime_error("NVRHI failed to create a TAA history texture");
            return texture;
        }

        [[nodiscard]] HistorySet CreateHistorySet(uint32_t index)
        {
            const std::string suffix = " [" + std::to_string(index) + "]";
            return {
                .Color = CreateHistoryTexture(
                    nvrhi::Format::RGBA16_FLOAT,
                    "TAA Color History" + suffix),
                .ViewDepth = CreateHistoryTexture(
                    nvrhi::Format::R32_FLOAT,
                    "TAA View Depth History" + suffix),
                .NormalLength = CreateHistoryTexture(
                    nvrhi::Format::RGBA16_FLOAT,
                    "TAA Normal History Length" + suffix),
                .SurfaceId = CreateHistoryTexture(
                    nvrhi::Format::RG32_UINT,
                    "TAA Surface ID History" + suffix),
            };
        }

        void RetireHistory()
        {
            if (!History[0].Color)
                return;
            RetiredHistories.push_back(RetiredHistory{
                .Submission = LastSubmittedSubmission,
                .Sets = std::move(History),
            });
            History = {};
        }

        void RetireTaaBindings()
        {
            if (TaaBindings.empty())
                return;
            RetiredResources retired{
                .Submission = LastSubmittedSubmission,
            };
            retired.BindingSets.reserve(TaaBindings.size());
            for (auto& [key, bindingSet] : TaaBindings)
            {
                (void)key;
                retired.BindingSets.push_back(std::move(bindingSet));
            }
            TaaBindings.clear();
            RetiredGpuResources.push_back(std::move(retired));
        }

        void RetireFxaaCaches()
        {
            if (FxaaBindings.empty() && FxaaFramebuffers.empty())
                return;
            RetiredResources retired{
                .Submission = LastSubmittedSubmission,
            };
            retired.BindingSets.reserve(FxaaBindings.size());
            for (auto& [texture, bindingSet] : FxaaBindings)
            {
                (void)texture;
                retired.BindingSets.push_back(std::move(bindingSet));
            }
            retired.Framebuffers.reserve(FxaaFramebuffers.size());
            for (auto& [texture, framebuffer] : FxaaFramebuffers)
            {
                (void)texture;
                retired.Framebuffers.push_back(std::move(framebuffer));
            }
            FxaaBindings.clear();
            FxaaFramebuffers.clear();
            RetiredGpuResources.push_back(std::move(retired));
        }

        void RetireSmaaCaches()
        {
            if (SmaaBindings.empty() && SmaaFramebuffers.empty())
                return;
            RetiredResources retired{
                .Submission = LastSubmittedSubmission,
            };
            retired.BindingSets.reserve(SmaaBindings.size());
            for (auto& [key, bindingSet] : SmaaBindings)
            {
                (void)key;
                retired.BindingSets.push_back(std::move(bindingSet));
            }
            retired.Framebuffers.reserve(SmaaFramebuffers.size());
            for (auto& [texture, framebuffer] : SmaaFramebuffers)
            {
                (void)texture;
                retired.Framebuffers.push_back(std::move(framebuffer));
            }
            SmaaBindings.clear();
            SmaaFramebuffers.clear();
            RetiredGpuResources.push_back(std::move(retired));
        }

        void AllocateHistory()
        {
            if (Width == 0 || Height == 0)
                throw std::logic_error("TAA history allocation requires non-zero dimensions");
            History = { CreateHistorySet(0), CreateHistorySet(1) };
            PreviousIndex = 0;
            CurrentIndex = 1;
            TemporalFrameSequence = 0;
            ConvergenceFrameCount = 0;
            bResetThisFrame = true;
            bGraphRebuildRequested = true;
        }

        [[nodiscard]] TextureHandle ImportHistoryTexture(
            RenderGraph& renderGraph,
            const nvrhi::TextureHandle& texture)
        {
            const nvrhi::TextureDesc& desc = texture->getDesc();
            return renderGraph.ImportTexture(
                RenderGraphTextureDesc{
                    .DebugName = desc.debugName,
                    .Width = desc.width,
                    .Height = desc.height,
                    .SampleCount = desc.sampleCount,
                    .Format = desc.format,
                    .Dimension = desc.dimension,
                },
                texture,
                nvrhi::ResourceStates::ShaderResource,
                nvrhi::ResourceStates::ShaderResource);
        }

        [[nodiscard]] TaaHistoryHandles ImportHistorySet(
            RenderGraph& renderGraph,
            const HistorySet& set)
        {
            return {
                .Color = ImportHistoryTexture(renderGraph, set.Color),
                .ViewDepth = ImportHistoryTexture(renderGraph, set.ViewDepth),
                .NormalLength = ImportHistoryTexture(
                    renderGraph,
                    set.NormalLength),
                .SurfaceId = ImportHistoryTexture(renderGraph, set.SurfaceId),
            };
        }

        void SetHistoryResources(
            RenderGraph& renderGraph,
            const TaaHistoryHandles& handles,
            const HistorySet& set)
        {
            renderGraph.SetTextureResource(handles.Color, set.Color);
            renderGraph.SetTextureResource(handles.ViewDepth, set.ViewDepth);
            renderGraph.SetTextureResource(
                handles.NormalLength,
                set.NormalLength);
            renderGraph.SetTextureResource(handles.SurfaceId, set.SurfaceId);
        }

        [[nodiscard]] nvrhi::IBindingSet* GetTaaBindingSet(
            const TaaBindingKey& key)
        {
            const auto existing = TaaBindings.find(key);
            if (existing != TaaBindings.end())
                return existing->second;
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::PushConstants(
                0,
                sizeof(TaaConstants)));
            for (uint32_t slot = 0; slot < 11; ++slot)
                desc.addItem(nvrhi::BindingSetItem::Texture_SRV(slot, key.Textures[slot]));
            for (uint32_t slot = 0; slot < 5; ++slot)
            {
                desc.addItem(nvrhi::BindingSetItem::Texture_UAV(
                    slot,
                    key.Textures[11 + slot]));
            }
            nvrhi::BindingSetHandle bindingSet = Device->createBindingSet(
                desc,
                TaaBindingLayout);
            if (!bindingSet)
                throw std::runtime_error("NVRHI failed to create a TAA binding set");
            return TaaBindings.emplace(key, std::move(bindingSet)).first->second;
        }

        [[nodiscard]] nvrhi::IBindingSet* GetFxaaBindingSet(
            nvrhi::ITexture* source)
        {
            const auto existing = FxaaBindings.find(source);
            if (existing != FxaaBindings.end())
                return existing->second;
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::PushConstants(
                    0,
                    sizeof(FxaaConstants)))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(0, source))
                .addItem(nvrhi::BindingSetItem::Sampler(0, LinearClampSampler));
            nvrhi::BindingSetHandle bindingSet = Device->createBindingSet(
                desc,
                FxaaBindingLayout);
            if (!bindingSet)
                throw std::runtime_error("NVRHI failed to create an FXAA binding set");
            return FxaaBindings.emplace(source, std::move(bindingSet)).first->second;
        }

        [[nodiscard]] nvrhi::IBindingSet* GetSmaaBindingSet(
            const SmaaBindingKey& key)
        {
            const auto existing = SmaaBindings.find(key);
            if (existing != SmaaBindings.end())
                return existing->second;
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::PushConstants(
                    0,
                    sizeof(SmaaConstants)))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(
                    0,
                    key.Textures[0]))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(
                    1,
                    key.Textures[1]))
                .addItem(nvrhi::BindingSetItem::Sampler(
                    0,
                    LinearClampSampler));
            nvrhi::BindingSetHandle bindingSet = Device->createBindingSet(
                desc,
                SmaaBindingLayout);
            if (!bindingSet)
                throw std::runtime_error("NVRHI failed to create an SMAA binding set");
            return SmaaBindings.emplace(key, std::move(bindingSet)).first->second;
        }

        [[nodiscard]] nvrhi::IFramebuffer* GetFxaaFramebuffer(
            nvrhi::ITexture* target)
        {
            const auto existing = FxaaFramebuffers.find(target);
            if (existing != FxaaFramebuffers.end())
                return existing->second;
            nvrhi::FramebufferDesc desc;
            desc.addColorAttachment(target);
            nvrhi::FramebufferHandle framebuffer = Device->createFramebuffer(desc);
            if (!framebuffer)
                throw std::runtime_error("NVRHI failed to create an FXAA framebuffer");
            return FxaaFramebuffers.emplace(
                target,
                std::move(framebuffer)).first->second;
        }

        [[nodiscard]] nvrhi::IFramebuffer* GetSmaaFramebuffer(
            nvrhi::ITexture* target)
        {
            const auto existing = SmaaFramebuffers.find(target);
            if (existing != SmaaFramebuffers.end())
                return existing->second;
            nvrhi::FramebufferDesc desc;
            desc.addColorAttachment(target);
            nvrhi::FramebufferHandle framebuffer = Device->createFramebuffer(desc);
            if (!framebuffer)
                throw std::runtime_error("NVRHI failed to create an SMAA framebuffer");
            return SmaaFramebuffers.emplace(
                target,
                std::move(framebuffer)).first->second;
        }

        [[nodiscard]] nvrhi::IGraphicsPipeline* GetFxaaPipeline(
            nvrhi::IFramebuffer* framebuffer)
        {
            if (FxaaPipeline)
                return FxaaPipeline;
            nvrhi::GraphicsPipelineDesc desc;
            desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setVertexShader(FxaaVertexShader)
                .setPixelShader(FxaaPixelShader)
                .addBindingLayout(FxaaBindingLayout);
            desc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
            desc.renderState.rasterState.setCullNone().enableDepthClip();
            FxaaPipeline = Device->createGraphicsPipeline(
                desc,
                framebuffer->getFramebufferInfo());
            if (!FxaaPipeline)
                throw std::runtime_error("NVRHI failed to create an FXAA pipeline");
            return FxaaPipeline;
        }

        [[nodiscard]] nvrhi::IGraphicsPipeline* GetSmaaPipeline(
            SmaaStage stage,
            nvrhi::IFramebuffer* framebuffer)
        {
            const size_t stageIndex = ToIndex(stage);
            if (stageIndex >= ToIndex(SmaaStage::Count))
                throw std::invalid_argument("Scene SMAA stage is invalid");
            if (SmaaPipelines[stageIndex])
                return SmaaPipelines[stageIndex];
            nvrhi::GraphicsPipelineDesc desc;
            desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setVertexShader(SmaaVertexShader)
                .setPixelShader(SmaaPixelShaders[stageIndex])
                .addBindingLayout(SmaaBindingLayout);
            desc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
            desc.renderState.rasterState.setCullNone().enableDepthClip();
            SmaaPipelines[stageIndex] = Device->createGraphicsPipeline(
                desc,
                framebuffer->getFramebufferInfo());
            if (!SmaaPipelines[stageIndex])
                throw std::runtime_error("NVRHI failed to create an SMAA pipeline");
            return SmaaPipelines[stageIndex];
        }

        void RecordTaa(
            RenderGraphContext& context,
            const TaaPassData& data)
        {
            const TaaBindingKey key{
                .Textures = {
                    context.GetTexture(
                        data.Inputs.CurrentHdrColor,
                        RenderGraphAccess::Read),
                    context.GetTexture(
                        data.Inputs.MotionVectors,
                        RenderGraphAccess::Read),
                    context.GetTexture(
                        data.Inputs.CurrentDepth,
                        RenderGraphAccess::Read),
                    context.GetTexture(
                        data.Inputs.ViewDepth,
                        RenderGraphAccess::Read),
                    context.GetTexture(
                        data.Inputs.NormalRoughness,
                        RenderGraphAccess::Read),
                    context.GetTexture(
                        data.Inputs.SurfaceId,
                        RenderGraphAccess::Read),
                    context.GetTexture(
                        data.Inputs.TemporalGiConfidence,
                        RenderGraphAccess::Read),
                    context.GetTexture(data.Previous.Color, RenderGraphAccess::Read),
                    context.GetTexture(
                        data.Previous.ViewDepth,
                        RenderGraphAccess::Read),
                    context.GetTexture(
                        data.Previous.NormalLength,
                        RenderGraphAccess::Read),
                    context.GetTexture(
                        data.Previous.SurfaceId,
                        RenderGraphAccess::Read),
                    context.GetTexture(data.Current.Color, RenderGraphAccess::Write),
                    context.GetTexture(
                        data.Current.ViewDepth,
                        RenderGraphAccess::Write),
                    context.GetTexture(
                        data.Current.NormalLength,
                        RenderGraphAccess::Write),
                    context.GetTexture(
                        data.Current.SurfaceId,
                        RenderGraphAccess::Write),
                    context.GetTexture(data.Resolved, RenderGraphAccess::Write),
                },
            };
            const TaaConstants constants{
                .Dimensions = { Width, Height },
                .HistoryWeight = PreparedSettings.TaaHistoryWeight,
                .Sharpness = PreparedSettings.TaaSharpness,
                .ResetHistory = bResetThisFrame ? 1u : 0u,
                .HasTemporalGiConfidence =
                    data.bHasTemporalGiConfidence ? 1u : 0u,
            };
            nvrhi::ComputeState state;
            state.setPipeline(TaaPipeline).addBindingSet(GetTaaBindingSet(key));
            nvrhi::ICommandList* commandList = context.GetCommandList();
            commandList->setComputeState(state);
            commandList->setPushConstants(&constants, sizeof(constants));
            commandList->dispatch(DispatchCount(Width), DispatchCount(Height));
            bFrameRecorded = true;
        }

        void RecordFxaa(
            RenderGraphContext& context,
            const FxaaPassData& data,
            const AntiAliasingSettings& settings,
            uint32_t width,
            uint32_t height)
        {
            nvrhi::ITexture* source = context.GetTexture(
                data.Source,
                RenderGraphAccess::Read);
            nvrhi::ITexture* target = context.GetTexture(
                data.Target,
                RenderGraphAccess::Write);
            nvrhi::IFramebuffer* framebuffer = GetFxaaFramebuffer(target);
            nvrhi::GraphicsState state;
            state.setPipeline(GetFxaaPipeline(framebuffer))
                .setFramebuffer(framebuffer)
                .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                    nvrhi::Viewport(
                        static_cast<float>(width),
                        static_cast<float>(height))))
                .addBindingSet(GetFxaaBindingSet(source));
            const FxaaConstants constants{
                .Dimensions = { width, height },
                .EdgeThreshold = settings.FxaaEdgeThreshold,
                .EdgeThresholdMin = settings.FxaaEdgeThresholdMin,
                .SubpixelQuality = settings.FxaaSubpixelQuality,
            };
            nvrhi::ICommandList* commandList = context.GetCommandList();
            commandList->setGraphicsState(state);
            commandList->setPushConstants(&constants, sizeof(constants));
            commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
        }

        void RecordSmaa(
            RenderGraphContext& context,
            const SmaaPassData& data,
            const AntiAliasingSettings& settings,
            uint32_t width,
            uint32_t height)
        {
            nvrhi::ITexture* source0 = context.GetTexture(
                data.Source0,
                RenderGraphAccess::Read);
            nvrhi::ITexture* source1 = context.GetTexture(
                data.Source1,
                RenderGraphAccess::Read);
            nvrhi::ITexture* target = context.GetTexture(
                data.Target,
                RenderGraphAccess::Write);
            nvrhi::IFramebuffer* framebuffer = GetSmaaFramebuffer(target);
            nvrhi::GraphicsState state;
            state.setPipeline(GetSmaaPipeline(data.Stage, framebuffer))
                .setFramebuffer(framebuffer)
                .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                    nvrhi::Viewport(
                        static_cast<float>(width),
                        static_cast<float>(height))))
                .addBindingSet(GetSmaaBindingSet(SmaaBindingKey{
                    .Textures = { source0, source1 },
                }));
            const SmaaConstants constants{
                .Dimensions = { width, height },
                .EdgeThreshold = settings.SmaaEdgeThreshold,
                .MaxSearchSteps = settings.SmaaMaxSearchSteps,
                .MaxDiagonalSearchSteps =
                    settings.SmaaMaxDiagonalSearchSteps,
                .CornerRounding = settings.SmaaCornerRounding,
            };
            nvrhi::ICommandList* commandList = context.GetCommandList();
            commandList->setGraphicsState(state);
            commandList->setPushConstants(&constants, sizeof(constants));
            commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
        }

        nvrhi::DeviceHandle Device;
        Scope<ShaderManager> OwnedShaderManager;
        ShaderManager& ShaderManagerInstance;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
        ShaderBinaryFormat BinaryFormat = ShaderBinaryFormat::Dxil;
        nvrhi::ShaderHandle FxaaVertexShader;
        nvrhi::ShaderHandle FxaaPixelShader;
        nvrhi::ShaderHandle TaaComputeShader;
        nvrhi::ShaderHandle SmaaVertexShader;
        std::array<
            nvrhi::ShaderHandle,
            ToIndex(SmaaStage::Count)> SmaaPixelShaders;
        nvrhi::BindingLayoutHandle FxaaBindingLayout;
        nvrhi::BindingLayoutHandle SmaaBindingLayout;
        nvrhi::BindingLayoutHandle TaaBindingLayout;
        nvrhi::SamplerHandle LinearClampSampler;
        nvrhi::GraphicsPipelineHandle FxaaPipeline;
        std::array<
            nvrhi::GraphicsPipelineHandle,
            ToIndex(SmaaStage::Count)> SmaaPipelines;
        nvrhi::ComputePipelineHandle TaaPipeline;
        std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> FxaaBindings;
        std::unordered_map<nvrhi::ITexture*, nvrhi::FramebufferHandle> FxaaFramebuffers;
        std::unordered_map<
            SmaaBindingKey,
            nvrhi::BindingSetHandle,
            SmaaBindingKeyHash> SmaaBindings;
        std::unordered_map<nvrhi::ITexture*, nvrhi::FramebufferHandle>
            SmaaFramebuffers;
        std::unordered_map<TaaBindingKey, nvrhi::BindingSetHandle, TaaBindingKeyHash>
            TaaBindings;
        std::array<HistorySet, 2> History;
        std::vector<RetiredHistory> RetiredHistories;
        std::vector<RetiredResources> RetiredGpuResources;
        TaaHistoryHandles PreviousGraphHandles;
        TaaHistoryHandles CurrentGraphHandles;
        AntiAliasingSettings PreparedSettings;
        uint64_t LastSubmittedSubmission = 0;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t PreviousIndex = 0;
        uint32_t CurrentIndex = 1;
        uint32_t TemporalFrameSequence = 0;
        uint32_t ConvergenceFrameCount = 0;
        bool bPrepared = false;
        bool bResetPending = true;
        bool bResetThisFrame = true;
        bool bFrameRecorded = false;
        bool bGraphHandlesValid = false;
        bool bGraphRebuildRequested = true;
    };

    SceneAntiAliasingPass::SceneAntiAliasingPass(
        const SceneAntiAliasingPassDesc& desc)
        : mImpl(CreateScope<Impl>(desc))
    {
    }

    SceneAntiAliasingPass::~SceneAntiAliasingPass() = default;

    void SceneAntiAliasingPass::PrepareFrame(
        const AntiAliasingSettings& settings,
        uint32_t width,
        uint32_t height,
        uint64_t completedSubmission,
        bool bCameraReset)
    {
        ValidateAntiAliasingSettings(settings);
        if (width == 0 || height == 0)
            throw std::invalid_argument("Scene AA dimensions must be non-zero");
        if (mImpl->bFrameRecorded)
        {
            throw std::logic_error(
                "Scene AA PrepareFrame requires the previous frame to be submitted or aborted");
        }

        Collect(completedSubmission);
        const bool bModeChanged = mImpl->bPrepared &&
            mImpl->PreparedSettings.Mode != settings.Mode;
        mImpl->PreparedSettings = settings;
        mImpl->bPrepared = true;

        if (mImpl->Width != width || mImpl->Height != height)
            Resize(width, height, completedSubmission);
        if (bModeChanged)
        {
            mImpl->RetireFxaaCaches();
            mImpl->RetireSmaaCaches();
            mImpl->RetireTaaBindings();
            mImpl->bGraphRebuildRequested = true;
            mImpl->bResetThisFrame = true;
            mImpl->TemporalFrameSequence = 0;
            mImpl->ConvergenceFrameCount = 0;
        }

        if (settings.Mode == AntiAliasingMode::Taa)
        {
            if (!mImpl->History[0].Color)
                mImpl->AllocateHistory();
        }
        else if (mImpl->History[0].Color)
        {
            mImpl->RetireTaaBindings();
            mImpl->RetireHistory();
            mImpl->PreviousGraphHandles = {};
            mImpl->CurrentGraphHandles = {};
            mImpl->bGraphHandlesValid = false;
            mImpl->bGraphRebuildRequested = true;
        }

        mImpl->bResetThisFrame = mImpl->bResetThisFrame ||
            mImpl->bResetPending || bCameraReset;
        if (bCameraReset)
        {
            mImpl->TemporalFrameSequence = 0;
            mImpl->ConvergenceFrameCount = 0;
        }
        mImpl->bResetPending = false;
    }

    void SceneAntiAliasingPass::Resize(
        uint32_t width,
        uint32_t height,
        uint64_t completedSubmission)
    {
        if (width == 0 || height == 0)
            throw std::invalid_argument("Scene AA dimensions must be non-zero");
        if (mImpl->bFrameRecorded)
        {
            throw std::logic_error(
                "Scene AA resize requires the recorded frame to be submitted or aborted");
        }
        Collect(completedSubmission);
        if (mImpl->Width == width && mImpl->Height == height)
            return;

        mImpl->RetireFxaaCaches();
        mImpl->RetireSmaaCaches();
        mImpl->RetireTaaBindings();
        mImpl->RetireHistory();
        mImpl->PreviousGraphHandles = {};
        mImpl->CurrentGraphHandles = {};
        mImpl->bGraphHandlesValid = false;
        mImpl->TemporalFrameSequence = 0;
        mImpl->ConvergenceFrameCount = 0;
        mImpl->Width = width;
        mImpl->Height = height;
        mImpl->bResetThisFrame = true;
        mImpl->bGraphRebuildRequested = true;
        if (mImpl->bPrepared &&
            mImpl->PreparedSettings.Mode == AntiAliasingMode::Taa)
        {
            mImpl->AllocateHistory();
        }
    }

    void SceneAntiAliasingPass::ResetHistory() noexcept
    {
        mImpl->bResetPending = true;
        mImpl->TemporalFrameSequence = 0;
        mImpl->ConvergenceFrameCount = 0;
    }

    void SceneAntiAliasingPass::RestartConvergence() noexcept
    {
        mImpl->ConvergenceFrameCount = 0;
    }

    uint32_t SceneAntiAliasingPass::GetTemporalFrameIndex() const noexcept
    {
        return mImpl->TemporalFrameSequence;
    }

    bool SceneAntiAliasingPass::RequiresContinuousUpdate() const noexcept
    {
        return mImpl->bPrepared &&
            mImpl->PreparedSettings.Mode == AntiAliasingMode::Taa &&
            mImpl->ConvergenceFrameCount < c_TaaConvergenceFrameCount;
    }

    TextureHandle SceneAntiAliasingPass::AddTaaPasses(
        RenderGraph& renderGraph,
        const SceneTaaPassInputs& inputs)
    {
        if (!mImpl->bPrepared ||
            mImpl->PreparedSettings.Mode != AntiAliasingMode::Taa ||
            !mImpl->History[0].Color)
        {
            throw std::logic_error(
                "Scene TAA AddTaaPasses requires a prepared TAA frame");
        }

        const RenderGraphTextureDesc& currentColorDesc =
            RequireSingleSampleTexture(
                renderGraph,
                inputs.CurrentHdrColor,
                "current HDR color");
        RequireFormat(currentColorDesc, nvrhi::Format::RGBA16_FLOAT, "current HDR color");
        const RenderGraphTextureDesc& motionDesc = RequireSingleSampleTexture(
            renderGraph,
            inputs.MotionVectors,
            "motion vectors");
        if (motionDesc.Format != nvrhi::Format::RG16_FLOAT &&
            motionDesc.Format != nvrhi::Format::RG32_FLOAT)
        {
            throw RenderGraphValidationError(
                "Scene TAA motion vectors must use RG16_FLOAT or RG32_FLOAT");
        }
        const RenderGraphTextureDesc& currentDepthDesc =
            RequireSingleSampleTexture(
                renderGraph,
                inputs.CurrentDepth,
                "current depth");
        if (currentDepthDesc.Format != nvrhi::Format::D32 &&
            currentDepthDesc.Format != nvrhi::Format::R32_FLOAT)
        {
            throw RenderGraphValidationError(
                "Scene TAA current depth must use D32 or R32_FLOAT");
        }
        RequireFormat(
            RequireSingleSampleTexture(renderGraph, inputs.ViewDepth, "view depth"),
            nvrhi::Format::RG32_FLOAT,
            "view depth");
        RequireFormat(
            RequireSingleSampleTexture(
                renderGraph,
                inputs.NormalRoughness,
                "normal roughness"),
            nvrhi::Format::RGBA16_FLOAT,
            "normal roughness");
        RequireFormat(
            RequireSingleSampleTexture(renderGraph, inputs.SurfaceId, "surface ID"),
            nvrhi::Format::RG32_UINT,
            "surface ID");

        TextureHandle temporalGiConfidence = inputs.CurrentDepth;
        bool bHasTemporalGiConfidence = false;
        if (inputs.TemporalGiConfidence)
        {
            const RenderGraphTextureDesc& confidenceDesc =
                RequireSingleSampleTexture(
                    renderGraph,
                    inputs.TemporalGiConfidence,
                    "temporal GI confidence");
            const uint32_t confidenceWidth = confidenceDesc.bRelativeToGraph
                ? std::max(
                    1u,
                    static_cast<uint32_t>(
                        static_cast<float>(mImpl->Width) *
                        confidenceDesc.WidthScale))
                : confidenceDesc.Width;
            const uint32_t confidenceHeight = confidenceDesc.bRelativeToGraph
                ? std::max(
                    1u,
                    static_cast<uint32_t>(
                        static_cast<float>(mImpl->Height) *
                        confidenceDesc.HeightScale))
                : confidenceDesc.Height;
            const bool bFloatConfidence =
                confidenceDesc.Format == nvrhi::Format::R16_FLOAT ||
                confidenceDesc.Format == nvrhi::Format::R32_FLOAT;
            if (bFloatConfidence && confidenceWidth == mImpl->Width &&
                confidenceHeight == mImpl->Height)
            {
                temporalGiConfidence = inputs.TemporalGiConfidence;
                bHasTemporalGiConfidence = true;
            }
        }

        mImpl->RetireTaaBindings();
        mImpl->PreviousGraphHandles = mImpl->ImportHistorySet(
            renderGraph,
            mImpl->History[mImpl->PreviousIndex]);
        mImpl->CurrentGraphHandles = mImpl->ImportHistorySet(
            renderGraph,
            mImpl->History[mImpl->CurrentIndex]);
        mImpl->bGraphHandlesValid = true;

        const TaaHistoryHandles previous = mImpl->PreviousGraphHandles;
        const TaaHistoryHandles current = mImpl->CurrentGraphHandles;
        const TextureHandle resolved = renderGraph.CreateTexture(
            RenderGraphTextureDesc{
                .DebugName = "Scene TAA Resolved HDR",
                .Width = mImpl->Width,
                .Height = mImpl->Height,
                .Format = nvrhi::Format::RGBA16_FLOAT,
                .CapabilityUsage = RenderGraphUsage::ShaderResource |
                    RenderGraphUsage::RenderTarget,
            });
        renderGraph.AddPass<TaaPassData>(
            "SceneTAA",
            RenderGraphQueue::Compute,
            [inputs,
                temporalGiConfidence,
                bHasTemporalGiConfidence,
                previous,
                current,
                resolved](
                RenderGraphBuilder& builder,
                TaaPassData& data)
            {
                data.Inputs.CurrentHdrColor = builder.ShaderResource(
                    inputs.CurrentHdrColor);
                data.Inputs.MotionVectors = builder.ShaderResource(
                    inputs.MotionVectors);
                data.Inputs.CurrentDepth = builder.DepthRead(inputs.CurrentDepth);
                data.Inputs.ViewDepth = builder.ShaderResource(inputs.ViewDepth);
                data.Inputs.NormalRoughness = builder.ShaderResource(
                    inputs.NormalRoughness);
                data.Inputs.SurfaceId = builder.ShaderResource(inputs.SurfaceId);
                data.Inputs.TemporalGiConfidence = builder.ShaderResource(
                    temporalGiConfidence);
                data.Previous.Color = builder.ShaderResource(previous.Color);
                data.Previous.ViewDepth = builder.ShaderResource(
                    previous.ViewDepth);
                data.Previous.NormalLength = builder.ShaderResource(
                    previous.NormalLength);
                data.Previous.SurfaceId = builder.ShaderResource(
                    previous.SurfaceId);
                data.Current.Color = builder.Write(
                    current.Color,
                    RenderGraphUsage::UnorderedAccess);
                data.Current.ViewDepth = builder.Write(
                    current.ViewDepth,
                    RenderGraphUsage::UnorderedAccess);
                data.Current.NormalLength = builder.Write(
                    current.NormalLength,
                    RenderGraphUsage::UnorderedAccess);
                data.Current.SurfaceId = builder.Write(
                    current.SurfaceId,
                    RenderGraphUsage::UnorderedAccess);
                data.Resolved = builder.Write(
                    resolved,
                    RenderGraphUsage::UnorderedAccess);
                data.bHasTemporalGiConfidence = bHasTemporalGiConfidence;
            },
            [impl = mImpl.get()](
                RenderGraphContext& context,
                const TaaPassData& data)
            {
                impl->RecordTaa(context, data);
            });
        return resolved;
    }

    TextureHandle SceneAntiAliasingPass::AddFxaaPass(
        RenderGraph& renderGraph,
        TextureHandle ldrInput,
        TextureHandle ldrTarget,
        const AntiAliasingSettings& settings,
        uint32_t width,
        uint32_t height)
    {
        ValidateAntiAliasingSettings(settings);
        if (settings.Mode != AntiAliasingMode::Fxaa)
            throw std::invalid_argument("Scene FXAA pass requires FXAA mode");
        if (width == 0 || height == 0)
            throw std::invalid_argument("Scene FXAA dimensions must be non-zero");
        if (ldrInput == ldrTarget)
        {
            throw RenderGraphValidationError(
                "Scene FXAA source and target must be different textures");
        }
        RequireFormat(
            RequireSingleSampleTexture(renderGraph, ldrInput, "FXAA LDR source"),
            nvrhi::Format::RGBA8_UNORM,
            "FXAA LDR source");
        RequireFormat(
            RequireSingleSampleTexture(renderGraph, ldrTarget, "FXAA LDR target"),
            nvrhi::Format::RGBA8_UNORM,
            "FXAA LDR target");

        mImpl->RetireFxaaCaches();
        renderGraph.AddPass<FxaaPassData>(
            "SceneFXAA",
            RenderGraphQueue::Graphics,
            [ldrInput, ldrTarget](
                RenderGraphBuilder& builder,
                FxaaPassData& data)
            {
                data.Source = builder.ShaderResource(ldrInput);
                data.Target = builder.Write(
                    ldrTarget,
                    RenderGraphUsage::RenderTarget);
            },
            [impl = mImpl.get(), settings, width, height](
                RenderGraphContext& context,
                const FxaaPassData& data)
            {
                impl->RecordFxaa(context, data, settings, width, height);
            });
        return ldrTarget;
    }

    TextureHandle SceneAntiAliasingPass::AddSmaaPasses(
        RenderGraph& renderGraph,
        TextureHandle ldrInput,
        TextureHandle ldrTarget,
        const AntiAliasingSettings& settings,
        uint32_t width,
        uint32_t height)
    {
        ValidateAntiAliasingSettings(settings);
        if (settings.Mode != AntiAliasingMode::Smaa)
            throw std::invalid_argument("Scene SMAA passes require SMAA mode");
        if (width == 0 || height == 0)
            throw std::invalid_argument("Scene SMAA dimensions must be non-zero");
        if (ldrInput == ldrTarget)
        {
            throw RenderGraphValidationError(
                "Scene SMAA source and target must be different textures");
        }
        RequireFormat(
            RequireSingleSampleTexture(renderGraph, ldrInput, "SMAA LDR source"),
            nvrhi::Format::RGBA8_UNORM,
            "SMAA LDR source");
        RequireFormat(
            RequireSingleSampleTexture(renderGraph, ldrTarget, "SMAA LDR target"),
            nvrhi::Format::RGBA8_UNORM,
            "SMAA LDR target");

        const TextureHandle edgeTexture = renderGraph.CreateTexture(
            RenderGraphTextureDesc{
                .DebugName = "Scene SMAA Edges",
                .Width = width,
                .Height = height,
                .Format = nvrhi::Format::RG8_UNORM,
            });
        const TextureHandle blendWeightTexture = renderGraph.CreateTexture(
            RenderGraphTextureDesc{
                .DebugName = "Scene SMAA Blend Weights",
                .Width = width,
                .Height = height,
                .Format = nvrhi::Format::RGBA8_UNORM,
            });

        mImpl->RetireSmaaCaches();
        renderGraph.AddPass<SmaaPassData>(
            "SceneSMAAEdgeDetection",
            RenderGraphQueue::Graphics,
            [ldrInput, edgeTexture](
                RenderGraphBuilder& builder,
                SmaaPassData& data)
            {
                data.Source0 = builder.ShaderResource(ldrInput);
                data.Source1 = builder.ShaderResource(ldrInput);
                data.Target = builder.Write(
                    edgeTexture,
                    RenderGraphUsage::RenderTarget);
                data.Stage = SmaaStage::EdgeDetection;
            },
            [impl = mImpl.get(), settings, width, height](
                RenderGraphContext& context,
                const SmaaPassData& data)
            {
                impl->RecordSmaa(context, data, settings, width, height);
            });
        renderGraph.AddPass<SmaaPassData>(
            "SceneSMAABlendWeights",
            RenderGraphQueue::Graphics,
            [edgeTexture, blendWeightTexture](
                RenderGraphBuilder& builder,
                SmaaPassData& data)
            {
                data.Source0 = builder.ShaderResource(edgeTexture);
                data.Source1 = builder.ShaderResource(edgeTexture);
                data.Target = builder.Write(
                    blendWeightTexture,
                    RenderGraphUsage::RenderTarget);
                data.Stage = SmaaStage::BlendWeight;
            },
            [impl = mImpl.get(), settings, width, height](
                RenderGraphContext& context,
                const SmaaPassData& data)
            {
                impl->RecordSmaa(context, data, settings, width, height);
            });
        renderGraph.AddPass<SmaaPassData>(
            "SceneSMAANeighborhoodBlend",
            RenderGraphQueue::Graphics,
            [ldrInput, ldrTarget, blendWeightTexture](
                RenderGraphBuilder& builder,
                SmaaPassData& data)
            {
                data.Source0 = builder.ShaderResource(ldrInput);
                data.Source1 = builder.ShaderResource(blendWeightTexture);
                data.Target = builder.Write(
                    ldrTarget,
                    RenderGraphUsage::RenderTarget);
                data.Stage = SmaaStage::Neighborhood;
            },
            [impl = mImpl.get(), settings, width, height](
                RenderGraphContext& context,
                const SmaaPassData& data)
            {
                impl->RecordSmaa(context, data, settings, width, height);
            });
        return ldrTarget;
    }

    void SceneAntiAliasingPass::UpdateImportedResources(
        RenderGraph& renderGraph)
    {
        if (!mImpl->bGraphHandlesValid || !mImpl->History[0].Color)
            return;
        mImpl->SetHistoryResources(
            renderGraph,
            mImpl->PreviousGraphHandles,
            mImpl->History[mImpl->PreviousIndex]);
        mImpl->SetHistoryResources(
            renderGraph,
            mImpl->CurrentGraphHandles,
            mImpl->History[mImpl->CurrentIndex]);
    }

    bool SceneAntiAliasingPass::ConsumeGraphRebuildRequest() noexcept
    {
        return std::exchange(mImpl->bGraphRebuildRequested, false);
    }

    void SceneAntiAliasingPass::NotifyFrameSubmitted(uint64_t submission) noexcept
    {
        mImpl->LastSubmittedSubmission = std::max(
            mImpl->LastSubmittedSubmission,
            submission);
        if (!mImpl->bFrameRecorded)
            return;
        mImpl->PreviousIndex = mImpl->CurrentIndex;
        mImpl->CurrentIndex = 1u - mImpl->PreviousIndex;
        mImpl->TemporalFrameSequence += 1u;
        mImpl->ConvergenceFrameCount = std::min(
            mImpl->ConvergenceFrameCount + 1u,
            c_TaaConvergenceFrameCount);
        mImpl->bFrameRecorded = false;
        mImpl->bResetThisFrame = false;
    }

    void SceneAntiAliasingPass::AbortFrame() noexcept
    {
        mImpl->bFrameRecorded = false;
    }

    void SceneAntiAliasingPass::Collect(uint64_t completedSubmission)
    {
        std::erase_if(
            mImpl->RetiredHistories,
            [completedSubmission](const Impl::RetiredHistory& retired)
            {
                return retired.Submission <= completedSubmission;
            });
        std::erase_if(
            mImpl->RetiredGpuResources,
            [completedSubmission](const Impl::RetiredResources& retired)
            {
                return retired.Submission <= completedSubmission;
            });
    }

    void SceneAntiAliasingPass::Clear(uint64_t completedSubmission)
    {
        Collect(completedSubmission);
        mImpl->bFrameRecorded = false;
        mImpl->RetireFxaaCaches();
        mImpl->RetireSmaaCaches();
        mImpl->RetireTaaBindings();
        mImpl->RetireHistory();
        mImpl->PreviousGraphHandles = {};
        mImpl->CurrentGraphHandles = {};
        mImpl->Width = 0;
        mImpl->Height = 0;
        mImpl->PreviousIndex = 0;
        mImpl->CurrentIndex = 1;
        mImpl->TemporalFrameSequence = 0;
        mImpl->ConvergenceFrameCount = 0;
        mImpl->bPrepared = false;
        mImpl->bResetPending = true;
        mImpl->bResetThisFrame = true;
        mImpl->bGraphHandlesValid = false;
        mImpl->bGraphRebuildRequested = true;
        Collect(completedSubmission);
    }
}
