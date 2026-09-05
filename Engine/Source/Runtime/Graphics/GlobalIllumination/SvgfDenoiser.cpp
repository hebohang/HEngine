#include "Hepch.h"

#include "Runtime/Graphics/GlobalIllumination/SvgfDenoiser.h"

#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
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
        enum class SvgfShaderKind : uint8_t
        {
            Demodulate,
            Temporal,
            HistoryFix,
            Variance,
            Atrous,
            Upsample,
            Remodulate,
        };

        struct alignas(16) DemodulateConstants
        {
            glm::uvec2 TraceDimensions{ 0u };
            glm::uvec2 FullDimensions{ 0u };
            float AlbedoEpsilon = 1e-4f;
            glm::vec3 Padding{ 0.0f };
        };

        struct alignas(16) TemporalConstants
        {
            glm::uvec2 TraceDimensions{ 0u };
            glm::uvec2 FullDimensions{ 0u };
            uint32_t ResetHistory = 1;
            float DepthThreshold = 0.0015f;
            float NormalThreshold = 0.8f;
            float MinimumBlend = 0.01f;
            // An 8-sigma same-surface interval rejects only extreme fireflies;
            // the former min/max clamp biased positive Monte-Carlo histories.
            float NeighborhoodClampSigma = 8.0f;
            float ViewDepthRelativeThreshold = 0.01f;
            glm::vec2 Padding{ 0.0f };
        };

        struct alignas(16) HistoryFixConstants
        {
            glm::uvec2 Dimensions{ 0u };
            glm::uvec2 FullDimensions{ 0u };
            float DepthSigma = 0.0025f;
            float NormalPower = 32.0f;
            float AlbedoSigma = 0.1f;
            float RoughnessSigma = 0.1f;
        };

        struct alignas(16) VarianceConstants
        {
            glm::uvec2 Dimensions{ 0u };
            glm::vec2 Padding{ 0.0f };
        };

        struct alignas(16) AtrousConstants
        {
            glm::uvec2 Dimensions{ 0u };
            glm::uvec2 FullDimensions{ 0u };
            uint32_t StepWidth = 1;
            float PhiDepth = 0.01f;
            float PhiNormal = 64.0f;
            // Illumination-space filtering must preserve direct/indirect
            // visibility edges even when depth, normal and material match.
            float PhiLuminance = 1.0f;
            float PhiAlbedo = 0.1f;
            float PhiRoughness = 0.1f;
            glm::vec2 Padding{ 0.0f };
        };

        struct alignas(16) UpsampleConstants
        {
            glm::uvec2 LowDimensions{ 0u };
            glm::uvec2 FullDimensions{ 0u };
            float DepthSigma = 0.0025f;
            float NormalPower = 32.0f;
            float AlbedoSigma = 0.1f;
            float RoughnessSigma = 0.1f;
        };

        struct alignas(16) RemodulateConstants
        {
            glm::uvec2 Dimensions{ 0u };
            float AlbedoEpsilon = 1e-4f;
            float Padding = 0.0f;
        };

        static_assert(sizeof(DemodulateConstants) == 32);
        static_assert(sizeof(TemporalConstants) == 48);
        static_assert(sizeof(HistoryFixConstants) == 32);
        static_assert(sizeof(VarianceConstants) == 16);
        static_assert(sizeof(AtrousConstants) == 48);
        static_assert(sizeof(UpsampleConstants) == 32);
        static_assert(sizeof(RemodulateConstants) == 16);

        struct SvgfHistoryHandles
        {
            TextureHandle Diffuse;
            TextureHandle Moments;
            TextureHandle Length;
            TextureHandle Depth;
            TextureHandle Normal;
            TextureHandle Material;
            TextureHandle HitDistance;
            TextureHandle ViewDepth;
            TextureHandle SurfaceId;
        };

        struct TemporalPassData
        {
            TextureHandle NoisyDiffuse;
            TextureHandle HitDistance;
            SceneSurfaceData Surface;
            TextureHandle TraceConfidence;
            TextureHandle Coverage;
            SvgfHistoryHandles Previous;
            SvgfHistoryHandles Current;
            TextureHandle Confidence;
        };

        struct DemodulatePassData
        {
            TextureHandle RawDiffuse;
            TextureHandle BaseColorMetalness;
            TextureHandle Emissive;
            TextureHandle Illumination;
        };

        struct HistoryFixPassData
        {
            TextureHandle Input;
            TextureHandle HistoryLength;
            TextureHandle Depth;
            TextureHandle Normal;
            TextureHandle Material;
            TextureHandle BaseColorMetalness;
            TextureHandle NormalRoughness;
            TextureHandle Output;
        };

        struct VariancePassData
        {
            TextureHandle Moments;
            TextureHandle HistoryLength;
            TextureHandle Variance;
        };

        struct AtrousPassData
        {
            TextureHandle Input;
            TextureHandle Variance;
            TextureHandle Depth;
            TextureHandle Normal;
            TextureHandle HitDistance;
            TextureHandle Material;
            TextureHandle BaseColorMetalness;
            TextureHandle NormalRoughness;
            TextureHandle Confidence;
            TextureHandle HistoryLength;
            TextureHandle Output;
            uint32_t Iteration = 0;
        };

        struct UpsamplePassData
        {
            TextureHandle LowDiffuse;
            TextureHandle FullDepth;
            TextureHandle FullNormal;
            TextureHandle LowDepth;
            TextureHandle LowNormal;
            TextureHandle FullBaseColorMetalness;
            TextureHandle FullMaterial;
            TextureHandle LowMaterial;
            TextureHandle LowHitDistance;
            TextureHandle LowConfidence;
            TextureHandle LowCoverage;
            TextureHandle FullDiffuse;
            TextureHandle FullHitDistance;
            TextureHandle FullConfidence;
            TextureHandle FullCoverage;
        };

        struct RemodulatePassData
        {
            TextureHandle Illumination;
            TextureHandle BaseColorMetalness;
            TextureHandle Emissive;
            TextureHandle DiffuseRadiance;
        };

        [[nodiscard]] std::string ReadShaderContentHash(
            const std::filesystem::path& path)
        {
            std::ifstream stream(path);
            if (!stream)
            {
                throw std::runtime_error(
                    "Failed to open SVGF shader cache-key sidecar: " +
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
                "SVGF shader cache-key sidecar has no key: " +
                Utils::PathToUtf8(path));
        }

        struct BindingKey
        {
            SvgfShaderKind Kind = SvgfShaderKind::Demodulate;
            std::array<nvrhi::ITexture*, 32> Textures{};
            uint8_t TextureCount = 0;

            [[nodiscard]] bool operator==(const BindingKey&) const = default;
        };

        struct BindingKeyHash
        {
            [[nodiscard]] size_t operator()(const BindingKey& key) const noexcept
            {
                size_t hash = static_cast<size_t>(key.Kind);
                for (uint32_t index = 0; index < key.TextureCount; ++index)
                {
                    hash ^= reinterpret_cast<size_t>(key.Textures[index]) +
                        0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                }
                return hash;
            }
        };

        [[nodiscard]] uint32_t DispatchCount(uint32_t value) noexcept
        {
            return (value + 7u) / 8u;
        }
    }

    struct SvgfDenoiser::Impl
    {
        struct HistorySet
        {
            nvrhi::TextureHandle Diffuse;
            nvrhi::TextureHandle Moments;
            nvrhi::TextureHandle Length;
            nvrhi::TextureHandle Depth;
            nvrhi::TextureHandle Normal;
            nvrhi::TextureHandle Material;
            nvrhi::TextureHandle HitDistance;
            nvrhi::TextureHandle ViewDepth;
            nvrhi::TextureHandle SurfaceId;
        };

        struct RetiredHistory
        {
            uint64_t Submission = 0;
            std::array<HistorySet, 2> Sets;
        };

        explicit Impl(const SvgfDenoiserDesc& desc)
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
                throw std::invalid_argument("SvgfDenoiser requires a valid NVRHI device");
            if (ShaderArtifactCacheRoot.empty())
                throw std::invalid_argument("SvgfDenoiser requires a shader artifact cache root");
            CreatePipelines();
        }

        [[nodiscard]] nvrhi::ShaderHandle LoadVariant(
            std::string_view variant)
        {
            const std::string fileName = "RtgiSvgf_" + std::string(variant) + ".cs";
            const std::filesystem::path path =
                ShaderArtifactCacheRoot /
                std::string(GetShaderArtifactDirectoryName(BinaryFormat)) /
                fileName;
            return ShaderManagerInstance.LoadShaderFile(
                ShaderCacheKey{
                    .Source = "GlobalIllumination/RtgiSvgf.hlsl",
                    .Variant = std::string(variant),
                    .Stage = nvrhi::ShaderType::Compute,
                    .EntryPoint = "CS",
                    .TargetProfile = "cs_6_0",
                    .ContentHash = ReadShaderContentHash(
                        MakeShaderCacheKeyPath(path)),
                    .Backend = Backend,
                    .BinaryFormat = BinaryFormat,
                },
                path,
                "SVGF " + std::string(variant));
        }

        [[nodiscard]] nvrhi::BindingLayoutHandle CreateLayout(
            uint32_t constantSize,
            uint32_t srvCount,
            uint32_t uavCount)
        {
            nvrhi::BindingLayoutDesc desc;
            desc.setVisibility(nvrhi::ShaderType::Compute)
                .setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0, constantSize));
            for (uint32_t slot = 0; slot < srvCount; ++slot)
                desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot));
            for (uint32_t slot = 0; slot < uavCount; ++slot)
                desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(slot));
            nvrhi::BindingLayoutHandle layout = Device->createBindingLayout(desc);
            if (!layout)
                throw std::runtime_error("NVRHI failed to create an SVGF binding layout");
            return layout;
        }

        [[nodiscard]] nvrhi::ComputePipelineHandle CreatePipeline(
            nvrhi::IShader* shader,
            nvrhi::IBindingLayout* layout)
        {
            nvrhi::ComputePipelineDesc desc;
            desc.setComputeShader(shader).addBindingLayout(layout);
            nvrhi::ComputePipelineHandle pipeline = Device->createComputePipeline(desc);
            if (!pipeline)
                throw std::runtime_error("NVRHI failed to create an SVGF compute pipeline");
            return pipeline;
        }

        void CreatePipelines()
        {
            DemodulateShader = LoadVariant("DEMODULATE");
            TemporalShader = LoadVariant("TEMPORAL");
            HistoryFixShader = LoadVariant("HISTORY_FIX");
            VarianceShader = LoadVariant("VARIANCE");
            AtrousShader = LoadVariant("ATROUS");
            UpsampleShader = LoadVariant("UPSAMPLE");
            RemodulateShader = LoadVariant("REMODULATE");
            DemodulateLayout = CreateLayout(sizeof(DemodulateConstants), 3, 1);
            TemporalLayout = CreateLayout(sizeof(TemporalConstants), 20, 10);
            HistoryFixLayout = CreateLayout(sizeof(HistoryFixConstants), 7, 1);
            VarianceLayout = CreateLayout(sizeof(VarianceConstants), 2, 1);
            AtrousLayout = CreateLayout(sizeof(AtrousConstants), 10, 1);
            UpsampleLayout = CreateLayout(sizeof(UpsampleConstants), 11, 4);
            RemodulateLayout = CreateLayout(sizeof(RemodulateConstants), 3, 1);
            DemodulatePipeline = CreatePipeline(DemodulateShader, DemodulateLayout);
            TemporalPipeline = CreatePipeline(TemporalShader, TemporalLayout);
            HistoryFixPipeline = CreatePipeline(HistoryFixShader, HistoryFixLayout);
            VariancePipeline = CreatePipeline(VarianceShader, VarianceLayout);
            AtrousPipeline = CreatePipeline(AtrousShader, AtrousLayout);
            UpsamplePipeline = CreatePipeline(UpsampleShader, UpsampleLayout);
            RemodulatePipeline = CreatePipeline(RemodulateShader, RemodulateLayout);
        }

        [[nodiscard]] nvrhi::TextureHandle CreateHistoryTexture(
            nvrhi::Format format,
            std::string debugName)
        {
            nvrhi::TextureDesc desc;
            desc.setWidth(TraceWidth)
                .setHeight(TraceHeight)
                .setFormat(format)
                .setIsUAV(true)
                .setInitialState(nvrhi::ResourceStates::ShaderResource)
                .setDebugName(debugName);
            nvrhi::TextureHandle texture = Device->createTexture(desc);
            if (!texture)
                throw std::runtime_error("NVRHI failed to create " + debugName);
            return texture;
        }

        [[nodiscard]] HistorySet CreateHistorySet(uint32_t index)
        {
            const std::string suffix = " [" + std::to_string(index) + "]";
            return {
                .Diffuse = CreateHistoryTexture(
                    nvrhi::Format::RGBA16_FLOAT,
                    "SVGF Diffuse History" + suffix),
                .Moments = CreateHistoryTexture(
                    nvrhi::Format::RG16_FLOAT,
                    "SVGF Moments History" + suffix),
                .Length = CreateHistoryTexture(
                    nvrhi::Format::R32_FLOAT,
                    "SVGF History Length" + suffix),
                .Depth = CreateHistoryTexture(
                    nvrhi::Format::R32_FLOAT,
                    "SVGF Depth History" + suffix),
                .Normal = CreateHistoryTexture(
                    nvrhi::Format::RGBA16_FLOAT,
                    "SVGF Normal History" + suffix),
                .Material = CreateHistoryTexture(
                    nvrhi::Format::R32_UINT,
                    "SVGF Material History" + suffix),
                .HitDistance = CreateHistoryTexture(
                    nvrhi::Format::R32_FLOAT,
                    "SVGF Hit Distance History" + suffix),
                .ViewDepth = CreateHistoryTexture(
                    nvrhi::Format::R32_FLOAT,
                    "SVGF View Depth History" + suffix),
                .SurfaceId = CreateHistoryTexture(
                    nvrhi::Format::RG32_UINT,
                    "SVGF Surface ID History" + suffix),
            };
        }

        void CreateHistory(uint32_t traceWidth, uint32_t traceHeight)
        {
            if (History[0].Diffuse)
            {
                RetiredHistories.push_back(RetiredHistory{
                    .Submission = LastSubmittedSubmission,
                    .Sets = std::move(History),
                });
            }
            TraceWidth = traceWidth;
            TraceHeight = traceHeight;
            History = { CreateHistorySet(0), CreateHistorySet(1) };
            PreviousIndex = 0;
            CurrentIndex = 1;
            bResetThisFrame = true;
            bGraphRebuildRequested = true;
            Bindings.clear();
        }

        [[nodiscard]] TextureHandle ImportHistoryTexture(
            RenderGraph& graph,
            const nvrhi::TextureHandle& texture)
        {
            const nvrhi::TextureDesc& desc = texture->getDesc();
            return graph.ImportTexture(
                RenderGraphTextureDesc{
                    .DebugName = desc.debugName,
                    .Width = desc.width,
                    .Height = desc.height,
                    .Format = desc.format,
                },
                texture,
                nvrhi::ResourceStates::ShaderResource,
                nvrhi::ResourceStates::ShaderResource);
        }

        [[nodiscard]] SvgfHistoryHandles ImportHistorySet(
            RenderGraph& graph,
            const HistorySet& set)
        {
            return {
                .Diffuse = ImportHistoryTexture(graph, set.Diffuse),
                .Moments = ImportHistoryTexture(graph, set.Moments),
                .Length = ImportHistoryTexture(graph, set.Length),
                .Depth = ImportHistoryTexture(graph, set.Depth),
                .Normal = ImportHistoryTexture(graph, set.Normal),
                .Material = ImportHistoryTexture(graph, set.Material),
                .HitDistance = ImportHistoryTexture(graph, set.HitDistance),
                .ViewDepth = ImportHistoryTexture(graph, set.ViewDepth),
                .SurfaceId = ImportHistoryTexture(graph, set.SurfaceId),
            };
        }

        void SetHistorySetResources(
            RenderGraph& graph,
            const SvgfHistoryHandles& handles,
            const HistorySet& set)
        {
            graph.SetTextureResource(handles.Diffuse, set.Diffuse);
            graph.SetTextureResource(handles.Moments, set.Moments);
            graph.SetTextureResource(handles.Length, set.Length);
            graph.SetTextureResource(handles.Depth, set.Depth);
            graph.SetTextureResource(handles.Normal, set.Normal);
            graph.SetTextureResource(handles.Material, set.Material);
            graph.SetTextureResource(handles.HitDistance, set.HitDistance);
            graph.SetTextureResource(handles.ViewDepth, set.ViewDepth);
            graph.SetTextureResource(handles.SurfaceId, set.SurfaceId);
        }

        [[nodiscard]] nvrhi::IBindingSet* GetBindingSet(
            const BindingKey& key,
            nvrhi::IBindingLayout* layout,
            uint32_t constantSize,
            uint32_t srvCount,
            uint32_t uavCount)
        {
            const auto existing = Bindings.find(key);
            if (existing != Bindings.end())
                return existing->second;
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::PushConstants(0, constantSize));
            for (uint32_t slot = 0; slot < srvCount; ++slot)
                desc.addItem(nvrhi::BindingSetItem::Texture_SRV(slot, key.Textures[slot]));
            for (uint32_t slot = 0; slot < uavCount; ++slot)
            {
                desc.addItem(nvrhi::BindingSetItem::Texture_UAV(
                    slot,
                    key.Textures[srvCount + slot]));
            }
            nvrhi::BindingSetHandle bindingSet = Device->createBindingSet(desc, layout);
            if (!bindingSet)
                throw std::runtime_error("NVRHI failed to create an SVGF binding set");
            return Bindings.emplace(key, std::move(bindingSet)).first->second;
        }

        template<typename Constants>
        void Dispatch(
            nvrhi::ICommandList* commandList,
            nvrhi::IComputePipeline* pipeline,
            nvrhi::IBindingSet* bindingSet,
            const Constants& constants,
            uint32_t width,
            uint32_t height)
        {
            nvrhi::ComputeState state;
            state.setPipeline(pipeline).addBindingSet(bindingSet);
            commandList->setComputeState(state);
            commandList->setPushConstants(&constants, sizeof(constants));
            commandList->dispatch(DispatchCount(width), DispatchCount(height));
        }

        nvrhi::DeviceHandle Device;
        Scope<ShaderManager> OwnedShaderManager;
        ShaderManager& ShaderManagerInstance;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
        ShaderBinaryFormat BinaryFormat = ShaderBinaryFormat::Dxil;
        nvrhi::ShaderHandle DemodulateShader;
        nvrhi::ShaderHandle TemporalShader;
        nvrhi::ShaderHandle HistoryFixShader;
        nvrhi::ShaderHandle VarianceShader;
        nvrhi::ShaderHandle AtrousShader;
        nvrhi::ShaderHandle UpsampleShader;
        nvrhi::ShaderHandle RemodulateShader;
        nvrhi::BindingLayoutHandle DemodulateLayout;
        nvrhi::BindingLayoutHandle TemporalLayout;
        nvrhi::BindingLayoutHandle HistoryFixLayout;
        nvrhi::BindingLayoutHandle VarianceLayout;
        nvrhi::BindingLayoutHandle AtrousLayout;
        nvrhi::BindingLayoutHandle UpsampleLayout;
        nvrhi::BindingLayoutHandle RemodulateLayout;
        nvrhi::ComputePipelineHandle DemodulatePipeline;
        nvrhi::ComputePipelineHandle TemporalPipeline;
        nvrhi::ComputePipelineHandle HistoryFixPipeline;
        nvrhi::ComputePipelineHandle VariancePipeline;
        nvrhi::ComputePipelineHandle AtrousPipeline;
        nvrhi::ComputePipelineHandle UpsamplePipeline;
        nvrhi::ComputePipelineHandle RemodulatePipeline;
        std::unordered_map<BindingKey, nvrhi::BindingSetHandle, BindingKeyHash> Bindings;
        std::array<HistorySet, 2> History;
        std::vector<RetiredHistory> RetiredHistories;
        SvgfHistoryHandles PreviousGraphHandles;
        SvgfHistoryHandles CurrentGraphHandles;
        GlobalIlluminationHistorySignature Signature;
        uint64_t LastSubmittedSubmission = 0;
        uint32_t FullWidth = 0;
        uint32_t FullHeight = 0;
        uint32_t TraceWidth = 0;
        uint32_t TraceHeight = 0;
        uint32_t PreviousIndex = 0;
        uint32_t CurrentIndex = 1;
        bool bResetPending = true;
        bool bResetThisFrame = true;
        bool bFrameRecorded = false;
        bool bGraphHandlesValid = false;
        bool bGraphRebuildRequested = true;
        float TemporalMinimumBlend = 0.01f;
    };

    SvgfDenoiser::SvgfDenoiser(const SvgfDenoiserDesc& desc)
        : mImpl(CreateScope<Impl>(desc))
    {
    }

    SvgfDenoiser::~SvgfDenoiser() = default;

    void SvgfDenoiser::PrepareFrame(
        const GlobalIlluminationHistorySignature& signature,
        uint64_t completedSubmission)
    {
        Collect(completedSubmission);
        const uint32_t traceWidth = std::max(
            static_cast<uint32_t>(std::ceil(
                static_cast<float>(signature.Width) * signature.RenderScale)),
            1u);
        const uint32_t traceHeight = std::max(
            static_cast<uint32_t>(std::ceil(
                static_cast<float>(signature.Height) * signature.RenderScale)),
            1u);
        if (signature.Width == 0 || signature.Height == 0)
            throw std::invalid_argument("SVGF dimensions must be non-zero");
        if (!mImpl->History[0].Diffuse ||
            mImpl->TraceWidth != traceWidth || mImpl->TraceHeight != traceHeight)
        {
            mImpl->CreateHistory(traceWidth, traceHeight);
        }
        mImpl->FullWidth = signature.Width;
        mImpl->FullHeight = signature.Height;
        mImpl->Signature = signature;
        mImpl->bResetThisFrame = mImpl->bResetPending || mImpl->bResetThisFrame;
        mImpl->bResetPending = false;
    }

    GlobalIlluminationOutputs SvgfDenoiser::AddPasses(
        RenderGraph& renderGraph,
        const GlobalIlluminationNoisySignals& noisySignals,
        const GlobalIlluminationDenoiserGuides& guides,
        const GlobalIlluminationSettings& settings)
    {
        if (!mImpl->History[0].Diffuse)
            throw std::logic_error("SVGF AddPasses requires PrepareFrame");
        if (!noisySignals.DiffuseIndirect || !noisySignals.HitDistance ||
            !guides.Surface || !guides.Confidence || !guides.Coverage)
        {
            throw std::invalid_argument("SVGF inputs are incomplete");
        }
        if (settings.Denoiser != GlobalIlluminationDenoiserKind::Svgf)
            throw std::invalid_argument("SVGF received a non-SVGF denoiser setting");
        mImpl->TemporalMinimumBlend = settings.Technique ==
                GlobalIlluminationTechniqueKind::ScreenSpace
            ? std::clamp(
                1.0f - settings.ScreenSpaceTemporalWeight,
                1.0f / 512.0f,
                1.0f)
            : 0.01f;

        mImpl->PreviousGraphHandles = mImpl->ImportHistorySet(
            renderGraph,
            mImpl->History[mImpl->PreviousIndex]);
        mImpl->CurrentGraphHandles = mImpl->ImportHistorySet(
            renderGraph,
            mImpl->History[mImpl->CurrentIndex]);
        mImpl->bGraphHandlesValid = true;

        TextureHandle demodulatedIllumination;
        renderGraph.AddPass<DemodulatePassData>(
            "DiffuseDemodulation",
            RenderGraphQueue::Compute,
            [impl = mImpl.get(), noisySignals, guides,
                &demodulatedIllumination](
                RenderGraphBuilder& builder,
                DemodulatePassData& data)
            {
                data.RawDiffuse = builder.ShaderResource(
                    noisySignals.DiffuseIndirect);
                data.BaseColorMetalness = builder.ShaderResource(
                    guides.Surface.BaseColorMetalness);
                data.Emissive = builder.ShaderResource(guides.Surface.Emissive);
                data.Illumination = builder.Write(builder.CreateTexture(
                    RenderGraphTextureDesc{
                        .DebugName = "SVGF Demodulated Diffuse Illumination",
                        .Width = impl->TraceWidth,
                        .Height = impl->TraceHeight,
                        .Format = nvrhi::Format::RGBA16_FLOAT,
                    }), RenderGraphUsage::UnorderedAccess);
                demodulatedIllumination = data.Illumination;
            },
            [impl = mImpl.get()](
                RenderGraphContext& context,
                const DemodulatePassData& data)
            {
                const BindingKey key{
                    .Kind = SvgfShaderKind::Demodulate,
                    .Textures = {
                        context.GetTexture(data.RawDiffuse, RenderGraphAccess::Read),
                        context.GetTexture(
                            data.BaseColorMetalness,
                            RenderGraphAccess::Read),
                        context.GetTexture(data.Emissive, RenderGraphAccess::Read),
                        context.GetTexture(data.Illumination, RenderGraphAccess::Write),
                    },
                    .TextureCount = 4,
                };
                const DemodulateConstants constants{
                    .TraceDimensions = { impl->TraceWidth, impl->TraceHeight },
                    .FullDimensions = { impl->FullWidth, impl->FullHeight },
                };
                impl->Dispatch(
                    context.GetCommandList(),
                    impl->DemodulatePipeline,
                    impl->GetBindingSet(
                        key,
                        impl->DemodulateLayout,
                        sizeof(constants),
                        3,
                        1),
                    constants,
                    impl->TraceWidth,
                    impl->TraceHeight);
            });

        TextureHandle temporalConfidence;
        renderGraph.AddPass<TemporalPassData>(
            "TemporalReprojection",
            RenderGraphQueue::Compute,
            [impl = mImpl.get(), noisySignals, guides,
                demodulatedIllumination, &temporalConfidence](
                RenderGraphBuilder& builder,
                TemporalPassData& data)
            {
                data.NoisyDiffuse = builder.ShaderResource(
                    demodulatedIllumination);
                data.HitDistance = builder.ShaderResource(noisySignals.HitDistance);
                data.Surface.Depth = builder.DepthRead(guides.Surface.Depth);
                data.Surface.NormalRoughness = builder.ShaderResource(
                    guides.Surface.NormalRoughness);
                data.Surface.BaseColorMetalness = builder.ShaderResource(
                    guides.Surface.BaseColorMetalness);
                data.Surface.Emissive = builder.ShaderResource(guides.Surface.Emissive);
                data.Surface.MotionVectors = builder.ShaderResource(
                    guides.Surface.MotionVectors);
                data.Surface.MaterialId = builder.ShaderResource(
                    guides.Surface.MaterialId);
                data.Surface.PreviousDepth = builder.ShaderResource(
                    guides.Surface.PreviousDepth);
                data.Surface.ViewDepth = builder.ShaderResource(
                    guides.Surface.ViewDepth);
                data.Surface.SurfaceId = builder.ShaderResource(
                    guides.Surface.SurfaceId);
                data.TraceConfidence = builder.ShaderResource(guides.Confidence);
                data.Coverage = builder.ShaderResource(guides.Coverage);
                data.Previous.Diffuse = builder.ShaderResource(
                    impl->PreviousGraphHandles.Diffuse);
                data.Previous.Moments = builder.ShaderResource(
                    impl->PreviousGraphHandles.Moments);
                data.Previous.Length = builder.ShaderResource(
                    impl->PreviousGraphHandles.Length);
                data.Previous.Depth = builder.ShaderResource(
                    impl->PreviousGraphHandles.Depth);
                data.Previous.Normal = builder.ShaderResource(
                    impl->PreviousGraphHandles.Normal);
                data.Previous.Material = builder.ShaderResource(
                    impl->PreviousGraphHandles.Material);
                data.Previous.HitDistance = builder.ShaderResource(
                    impl->PreviousGraphHandles.HitDistance);
                data.Previous.ViewDepth = builder.ShaderResource(
                    impl->PreviousGraphHandles.ViewDepth);
                data.Previous.SurfaceId = builder.ShaderResource(
                    impl->PreviousGraphHandles.SurfaceId);
                data.Current.Diffuse = builder.Write(
                    impl->CurrentGraphHandles.Diffuse,
                    RenderGraphUsage::UnorderedAccess);
                data.Current.Moments = builder.Write(
                    impl->CurrentGraphHandles.Moments,
                    RenderGraphUsage::UnorderedAccess);
                data.Current.Length = builder.Write(
                    impl->CurrentGraphHandles.Length,
                    RenderGraphUsage::UnorderedAccess);
                data.Current.Depth = builder.Write(
                    impl->CurrentGraphHandles.Depth,
                    RenderGraphUsage::UnorderedAccess);
                data.Current.Normal = builder.Write(
                    impl->CurrentGraphHandles.Normal,
                    RenderGraphUsage::UnorderedAccess);
                data.Current.Material = builder.Write(
                    impl->CurrentGraphHandles.Material,
                    RenderGraphUsage::UnorderedAccess);
                data.Current.HitDistance = builder.Write(
                    impl->CurrentGraphHandles.HitDistance,
                    RenderGraphUsage::UnorderedAccess);
                data.Current.ViewDepth = builder.Write(
                    impl->CurrentGraphHandles.ViewDepth,
                    RenderGraphUsage::UnorderedAccess);
                data.Current.SurfaceId = builder.Write(
                    impl->CurrentGraphHandles.SurfaceId,
                    RenderGraphUsage::UnorderedAccess);
                data.Confidence = builder.Write(builder.CreateTexture(
                    RenderGraphTextureDesc{
                        .DebugName = "SVGF Temporal Confidence",
                        .Width = impl->TraceWidth,
                        .Height = impl->TraceHeight,
                        .Format = nvrhi::Format::R32_FLOAT,
                    }), RenderGraphUsage::UnorderedAccess);
                temporalConfidence = data.Confidence;
            },
            [impl = mImpl.get()](
                RenderGraphContext& context,
                const TemporalPassData& data)
            {
                std::array<nvrhi::ITexture*, 32> textures{};
                textures[0] = context.GetTexture(data.NoisyDiffuse, RenderGraphAccess::Read);
                textures[1] = context.GetTexture(data.HitDistance, RenderGraphAccess::Read);
                textures[2] = context.GetTexture(data.Surface.Depth, RenderGraphAccess::Read);
                textures[3] = context.GetTexture(
                    data.Surface.NormalRoughness, RenderGraphAccess::Read);
                textures[4] = context.GetTexture(
                    data.Surface.MotionVectors, RenderGraphAccess::Read);
                textures[5] = context.GetTexture(
                    data.Surface.MaterialId, RenderGraphAccess::Read);
                textures[6] = context.GetTexture(data.TraceConfidence, RenderGraphAccess::Read);
                textures[7] = context.GetTexture(data.Previous.Diffuse, RenderGraphAccess::Read);
                textures[8] = context.GetTexture(data.Previous.Moments, RenderGraphAccess::Read);
                textures[9] = context.GetTexture(data.Previous.Length, RenderGraphAccess::Read);
                textures[10] = context.GetTexture(data.Previous.Depth, RenderGraphAccess::Read);
                textures[11] = context.GetTexture(data.Previous.Normal, RenderGraphAccess::Read);
                textures[12] = context.GetTexture(data.Previous.Material, RenderGraphAccess::Read);
                textures[13] = context.GetTexture(
                    data.Surface.PreviousDepth,
                    RenderGraphAccess::Read);
                textures[14] = context.GetTexture(
                    data.Previous.HitDistance,
                    RenderGraphAccess::Read);
                textures[15] = context.GetTexture(data.Coverage, RenderGraphAccess::Read);
                textures[16] = context.GetTexture(
                    data.Surface.ViewDepth,
                    RenderGraphAccess::Read);
                textures[17] = context.GetTexture(
                    data.Surface.SurfaceId,
                    RenderGraphAccess::Read);
                textures[18] = context.GetTexture(
                    data.Previous.ViewDepth,
                    RenderGraphAccess::Read);
                textures[19] = context.GetTexture(
                    data.Previous.SurfaceId,
                    RenderGraphAccess::Read);
                textures[20] = context.GetTexture(data.Current.Diffuse, RenderGraphAccess::Write);
                textures[21] = context.GetTexture(data.Current.Moments, RenderGraphAccess::Write);
                textures[22] = context.GetTexture(data.Current.Length, RenderGraphAccess::Write);
                textures[23] = context.GetTexture(data.Current.Depth, RenderGraphAccess::Write);
                textures[24] = context.GetTexture(data.Current.Normal, RenderGraphAccess::Write);
                textures[25] = context.GetTexture(data.Current.Material, RenderGraphAccess::Write);
                textures[26] = context.GetTexture(data.Confidence, RenderGraphAccess::Write);
                textures[27] = context.GetTexture(
                    data.Current.HitDistance,
                    RenderGraphAccess::Write);
                textures[28] = context.GetTexture(
                    data.Current.ViewDepth,
                    RenderGraphAccess::Write);
                textures[29] = context.GetTexture(
                    data.Current.SurfaceId,
                    RenderGraphAccess::Write);
                const BindingKey key{
                    .Kind = SvgfShaderKind::Temporal,
                    .Textures = textures,
                    .TextureCount = 30,
                };
                const TemporalConstants constants{
                    .TraceDimensions = { impl->TraceWidth, impl->TraceHeight },
                    .FullDimensions = { impl->FullWidth, impl->FullHeight },
                    .ResetHistory = impl->bResetThisFrame ? 1u : 0u,
                    .MinimumBlend = impl->TemporalMinimumBlend,
                };
                impl->Dispatch(
                    context.GetCommandList(),
                    impl->TemporalPipeline,
                    impl->GetBindingSet(
                        key,
                        impl->TemporalLayout,
                        sizeof(constants),
                        20,
                        10),
                    constants,
                    impl->TraceWidth,
                    impl->TraceHeight);
                impl->bFrameRecorded = true;
            });

        TextureHandle historyFixed;
        renderGraph.AddPass<HistoryFixPassData>(
            "HistoryFix",
            RenderGraphQueue::Compute,
            [impl = mImpl.get(), guides, &historyFixed](
                RenderGraphBuilder& builder,
                HistoryFixPassData& data)
            {
                data.Input = builder.ShaderResource(impl->CurrentGraphHandles.Diffuse);
                data.HistoryLength = builder.ShaderResource(
                    impl->CurrentGraphHandles.Length);
                data.Depth = builder.ShaderResource(impl->CurrentGraphHandles.Depth);
                data.Normal = builder.ShaderResource(impl->CurrentGraphHandles.Normal);
                data.Material = builder.ShaderResource(
                    impl->CurrentGraphHandles.Material);
                data.BaseColorMetalness = builder.ShaderResource(
                    guides.Surface.BaseColorMetalness);
                data.NormalRoughness = builder.ShaderResource(
                    guides.Surface.NormalRoughness);
                data.Output = builder.Write(builder.CreateTexture(
                    RenderGraphTextureDesc{
                        .DebugName = "SVGF History Fixed Diffuse",
                        .Width = impl->TraceWidth,
                        .Height = impl->TraceHeight,
                        .Format = nvrhi::Format::RGBA16_FLOAT,
                    }), RenderGraphUsage::UnorderedAccess);
                historyFixed = data.Output;
            },
            [impl = mImpl.get()](
                RenderGraphContext& context,
                const HistoryFixPassData& data)
            {
                const BindingKey key{
                    .Kind = SvgfShaderKind::HistoryFix,
                    .Textures = {
                        context.GetTexture(data.Input, RenderGraphAccess::Read),
                        context.GetTexture(data.HistoryLength, RenderGraphAccess::Read),
                        context.GetTexture(data.Depth, RenderGraphAccess::Read),
                        context.GetTexture(data.Normal, RenderGraphAccess::Read),
                        context.GetTexture(data.Material, RenderGraphAccess::Read),
                        context.GetTexture(
                            data.BaseColorMetalness,
                            RenderGraphAccess::Read),
                        context.GetTexture(
                            data.NormalRoughness,
                            RenderGraphAccess::Read),
                        context.GetTexture(data.Output, RenderGraphAccess::Write),
                    },
                    .TextureCount = 8,
                };
                const HistoryFixConstants constants{
                    .Dimensions = { impl->TraceWidth, impl->TraceHeight },
                    .FullDimensions = { impl->FullWidth, impl->FullHeight },
                };
                impl->Dispatch(
                    context.GetCommandList(),
                    impl->HistoryFixPipeline,
                    impl->GetBindingSet(
                        key,
                        impl->HistoryFixLayout,
                        sizeof(constants),
                        7,
                        1),
                    constants,
                    impl->TraceWidth,
                    impl->TraceHeight);
            });

        TextureHandle variance;
        renderGraph.AddPass<VariancePassData>(
            "MomentsAndVariance",
            RenderGraphQueue::Compute,
            [impl = mImpl.get(), &variance](
                RenderGraphBuilder& builder,
                VariancePassData& data)
            {
                data.Moments = builder.ShaderResource(impl->CurrentGraphHandles.Moments);
                data.HistoryLength = builder.ShaderResource(
                    impl->CurrentGraphHandles.Length);
                data.Variance = builder.Write(builder.CreateTexture(
                    RenderGraphTextureDesc{
                        .DebugName = "SVGF Luminance Variance",
                        .Width = impl->TraceWidth,
                        .Height = impl->TraceHeight,
                        .Format = nvrhi::Format::R32_FLOAT,
                    }), RenderGraphUsage::UnorderedAccess);
                variance = data.Variance;
            },
            [impl = mImpl.get()](
                RenderGraphContext& context,
                const VariancePassData& data)
            {
                const BindingKey key{
                    .Kind = SvgfShaderKind::Variance,
                    .Textures = {
                        context.GetTexture(data.Moments, RenderGraphAccess::Read),
                        context.GetTexture(data.HistoryLength, RenderGraphAccess::Read),
                        context.GetTexture(data.Variance, RenderGraphAccess::Write),
                    },
                    .TextureCount = 3,
                };
                const VarianceConstants constants{
                    .Dimensions = { impl->TraceWidth, impl->TraceHeight },
                };
                impl->Dispatch(
                    context.GetCommandList(),
                    impl->VariancePipeline,
                    impl->GetBindingSet(
                        key,
                        impl->VarianceLayout,
                        sizeof(constants),
                        2,
                        1),
                    constants,
                    impl->TraceWidth,
                    impl->TraceHeight);
            });

        TextureHandle filtered = historyFixed;
        const uint32_t iterationCount = std::clamp(settings.AtrousIterations, 1u, 5u);
        for (uint32_t iteration = 0; iteration < iterationCount; ++iteration)
        {
            TextureHandle output;
            renderGraph.AddPass<AtrousPassData>(
                "ATrousFilter[" + std::to_string(iteration) + "]",
                RenderGraphQueue::Compute,
                [impl = mImpl.get(), input = filtered, variance,
                    temporalConfidence,
                    guides, iteration, &output](
                    RenderGraphBuilder& builder,
                    AtrousPassData& data)
                {
                    data.Input = builder.ShaderResource(input);
                    data.Variance = builder.ShaderResource(variance);
                    data.Depth = builder.ShaderResource(impl->CurrentGraphHandles.Depth);
                    data.Normal = builder.ShaderResource(impl->CurrentGraphHandles.Normal);
                    data.HitDistance = builder.ShaderResource(
                        impl->CurrentGraphHandles.HitDistance);
                    data.Material = builder.ShaderResource(
                        impl->CurrentGraphHandles.Material);
                    data.BaseColorMetalness = builder.ShaderResource(
                        guides.Surface.BaseColorMetalness);
                    data.NormalRoughness = builder.ShaderResource(
                        guides.Surface.NormalRoughness);
                    data.Confidence = builder.ShaderResource(temporalConfidence);
                    data.HistoryLength = builder.ShaderResource(
                        impl->CurrentGraphHandles.Length);
                    data.Output = builder.Write(builder.CreateTexture(
                        RenderGraphTextureDesc{
                            .DebugName = "SVGF A-Trous " + std::to_string(iteration),
                            .Width = impl->TraceWidth,
                            .Height = impl->TraceHeight,
                            .Format = nvrhi::Format::RGBA16_FLOAT,
                        }), RenderGraphUsage::UnorderedAccess);
                    data.Iteration = iteration;
                    output = data.Output;
                },
                [impl = mImpl.get()](
                    RenderGraphContext& context,
                    const AtrousPassData& data)
                {
                    const BindingKey key{
                        .Kind = SvgfShaderKind::Atrous,
                        .Textures = {
                            context.GetTexture(data.Input, RenderGraphAccess::Read),
                            context.GetTexture(data.Variance, RenderGraphAccess::Read),
                            context.GetTexture(data.Depth, RenderGraphAccess::Read),
                            context.GetTexture(data.Normal, RenderGraphAccess::Read),
                            context.GetTexture(data.HitDistance, RenderGraphAccess::Read),
                            context.GetTexture(data.Material, RenderGraphAccess::Read),
                            context.GetTexture(
                                data.BaseColorMetalness,
                                RenderGraphAccess::Read),
                            context.GetTexture(
                                data.NormalRoughness,
                                RenderGraphAccess::Read),
                            context.GetTexture(data.Confidence, RenderGraphAccess::Read),
                            context.GetTexture(
                                data.HistoryLength,
                                RenderGraphAccess::Read),
                            context.GetTexture(data.Output, RenderGraphAccess::Write),
                        },
                        .TextureCount = 11,
                    };
                    const AtrousConstants constants{
                        .Dimensions = { impl->TraceWidth, impl->TraceHeight },
                        .FullDimensions = { impl->FullWidth, impl->FullHeight },
                        .StepWidth = 1u << data.Iteration,
                    };
                    impl->Dispatch(
                        context.GetCommandList(),
                        impl->AtrousPipeline,
                        impl->GetBindingSet(
                            key,
                            impl->AtrousLayout,
                            sizeof(constants),
                            10,
                            1),
                        constants,
                        impl->TraceWidth,
                        impl->TraceHeight);
                });
            filtered = output;
        }

        TextureHandle finalDiffuse = filtered;
        TextureHandle finalHitDistance = mImpl->CurrentGraphHandles.HitDistance;
        TextureHandle finalConfidence = temporalConfidence;
        TextureHandle finalCoverage = guides.Coverage;
        if (mImpl->TraceWidth != mImpl->FullWidth ||
            mImpl->TraceHeight != mImpl->FullHeight)
        {
            if (!settings.bBilateralUpsample)
            {
                throw std::invalid_argument(
                    "Half-resolution SVGF requires bilateral upsampling");
            }
            TextureHandle upsampled;
            renderGraph.AddPass<UpsamplePassData>(
                "BilateralUpsample",
                RenderGraphQueue::Compute,
                [impl = mImpl.get(), filtered, guides,
                    temporalConfidence, &upsampled,
                    &finalHitDistance, &finalConfidence, &finalCoverage](
                    RenderGraphBuilder& builder,
                    UpsamplePassData& data)
                {
                    data.LowDiffuse = builder.ShaderResource(filtered);
                    data.FullDepth = builder.DepthRead(guides.Surface.Depth);
                    data.FullNormal = builder.ShaderResource(
                        guides.Surface.NormalRoughness);
                    data.LowDepth = builder.ShaderResource(
                        impl->CurrentGraphHandles.Depth);
                    data.LowNormal = builder.ShaderResource(
                        impl->CurrentGraphHandles.Normal);
                    data.FullBaseColorMetalness = builder.ShaderResource(
                        guides.Surface.BaseColorMetalness);
                    data.FullMaterial = builder.ShaderResource(
                        guides.Surface.MaterialId);
                    data.LowMaterial = builder.ShaderResource(
                        impl->CurrentGraphHandles.Material);
                    data.LowHitDistance = builder.ShaderResource(
                        impl->CurrentGraphHandles.HitDistance);
                    data.LowConfidence = builder.ShaderResource(
                        temporalConfidence);
                    data.LowCoverage = builder.ShaderResource(guides.Coverage);
                    data.FullDiffuse = builder.Write(builder.CreateTexture(
                        RenderGraphTextureDesc{
                            .DebugName = "SVGF Bilateral Upsampled Diffuse",
                            .Width = impl->FullWidth,
                            .Height = impl->FullHeight,
                            .Format = nvrhi::Format::RGBA16_FLOAT,
                        }), RenderGraphUsage::UnorderedAccess);
                    upsampled = data.FullDiffuse;
                    data.FullHitDistance = builder.Write(builder.CreateTexture(
                        RenderGraphTextureDesc{
                            .DebugName = "SVGF Bilateral Upsampled Hit Distance",
                            .Width = impl->FullWidth,
                            .Height = impl->FullHeight,
                            .Format = nvrhi::Format::R32_FLOAT,
                        }), RenderGraphUsage::UnorderedAccess);
                    data.FullConfidence = builder.Write(builder.CreateTexture(
                        RenderGraphTextureDesc{
                            .DebugName = "SVGF Bilateral Upsampled Confidence",
                            .Width = impl->FullWidth,
                            .Height = impl->FullHeight,
                            .Format = nvrhi::Format::R32_FLOAT,
                        }), RenderGraphUsage::UnorderedAccess);
                    finalHitDistance = data.FullHitDistance;
                    finalConfidence = data.FullConfidence;
                    data.FullCoverage = builder.Write(builder.CreateTexture(
                        RenderGraphTextureDesc{
                            .DebugName = "SVGF Bilateral Upsampled Coverage",
                            .Width = impl->FullWidth,
                            .Height = impl->FullHeight,
                            .Format = nvrhi::Format::R32_FLOAT,
                        }), RenderGraphUsage::UnorderedAccess);
                    finalCoverage = data.FullCoverage;
                },
                [impl = mImpl.get()](
                    RenderGraphContext& context,
                    const UpsamplePassData& data)
                {
                    const BindingKey key{
                        .Kind = SvgfShaderKind::Upsample,
                        .Textures = {
                            context.GetTexture(data.LowDiffuse, RenderGraphAccess::Read),
                            context.GetTexture(data.FullDepth, RenderGraphAccess::Read),
                            context.GetTexture(data.FullNormal, RenderGraphAccess::Read),
                            context.GetTexture(data.LowDepth, RenderGraphAccess::Read),
                            context.GetTexture(data.LowNormal, RenderGraphAccess::Read),
                            context.GetTexture(
                                data.FullBaseColorMetalness,
                                RenderGraphAccess::Read),
                            context.GetTexture(
                                data.FullMaterial,
                                RenderGraphAccess::Read),
                            context.GetTexture(
                                data.LowMaterial,
                                RenderGraphAccess::Read),
                            context.GetTexture(
                                data.LowHitDistance,
                                RenderGraphAccess::Read),
                            context.GetTexture(
                                data.LowConfidence,
                                RenderGraphAccess::Read),
                            context.GetTexture(
                                data.LowCoverage,
                                RenderGraphAccess::Read),
                            context.GetTexture(data.FullDiffuse, RenderGraphAccess::Write),
                            context.GetTexture(
                                data.FullHitDistance,
                                RenderGraphAccess::Write),
                            context.GetTexture(
                                data.FullConfidence,
                                RenderGraphAccess::Write),
                            context.GetTexture(
                                data.FullCoverage,
                                RenderGraphAccess::Write),
                        },
                        .TextureCount = 15,
                    };
                    const UpsampleConstants constants{
                        .LowDimensions = { impl->TraceWidth, impl->TraceHeight },
                        .FullDimensions = { impl->FullWidth, impl->FullHeight },
                    };
                    impl->Dispatch(
                        context.GetCommandList(),
                        impl->UpsamplePipeline,
                        impl->GetBindingSet(
                            key,
                            impl->UpsampleLayout,
                            sizeof(constants),
                            11,
                            4),
                        constants,
                        impl->FullWidth,
                        impl->FullHeight);
                });
            finalDiffuse = upsampled;
        }

        TextureHandle remodulatedDiffuse;
        renderGraph.AddPass<RemodulatePassData>(
            "DiffuseRemodulation",
            RenderGraphQueue::Compute,
            [impl = mImpl.get(), finalDiffuse, guides, &remodulatedDiffuse](
                RenderGraphBuilder& builder,
                RemodulatePassData& data)
            {
                data.Illumination = builder.ShaderResource(finalDiffuse);
                data.BaseColorMetalness = builder.ShaderResource(
                    guides.Surface.BaseColorMetalness);
                data.Emissive = builder.ShaderResource(guides.Surface.Emissive);
                data.DiffuseRadiance = builder.Write(builder.CreateTexture(
                    RenderGraphTextureDesc{
                        .DebugName = "SVGF Remodulated Diffuse Radiance",
                        .Width = impl->FullWidth,
                        .Height = impl->FullHeight,
                        .Format = nvrhi::Format::RGBA16_FLOAT,
                    }), RenderGraphUsage::UnorderedAccess);
                remodulatedDiffuse = data.DiffuseRadiance;
            },
            [impl = mImpl.get()](
                RenderGraphContext& context,
                const RemodulatePassData& data)
            {
                const BindingKey key{
                    .Kind = SvgfShaderKind::Remodulate,
                    .Textures = {
                        context.GetTexture(data.Illumination, RenderGraphAccess::Read),
                        context.GetTexture(
                            data.BaseColorMetalness,
                            RenderGraphAccess::Read),
                        context.GetTexture(data.Emissive, RenderGraphAccess::Read),
                        context.GetTexture(
                            data.DiffuseRadiance,
                            RenderGraphAccess::Write),
                    },
                    .TextureCount = 4,
                };
                const RemodulateConstants constants{
                    .Dimensions = { impl->FullWidth, impl->FullHeight },
                };
                impl->Dispatch(
                    context.GetCommandList(),
                    impl->RemodulatePipeline,
                    impl->GetBindingSet(
                        key,
                        impl->RemodulateLayout,
                        sizeof(constants),
                        3,
                        1),
                    constants,
                    impl->FullWidth,
                    impl->FullHeight);
            });

        return {
            .DiffuseIndirect = remodulatedDiffuse,
            .DemodulatedDiffuseIllumination = demodulatedIllumination,
            .FilteredDiffuseIllumination = finalDiffuse,
            .HitDistance = finalHitDistance,
            .Confidence = finalConfidence,
            .CoverageMask = finalCoverage,
            .HistoryLength = mImpl->CurrentGraphHandles.Length,
            .Variance = variance,
        };
    }

    void SvgfDenoiser::ResetHistory(GlobalIlluminationHistoryResetReason reason)
    {
        (void)reason;
        mImpl->bResetPending = true;
    }

    void SvgfDenoiser::UpdateImportedResources(RenderGraph& renderGraph)
    {
        if (!mImpl->bGraphHandlesValid)
            return;
        mImpl->SetHistorySetResources(
            renderGraph,
            mImpl->PreviousGraphHandles,
            mImpl->History[mImpl->PreviousIndex]);
        mImpl->SetHistorySetResources(
            renderGraph,
            mImpl->CurrentGraphHandles,
            mImpl->History[mImpl->CurrentIndex]);
    }

    void SvgfDenoiser::NotifyFrameSubmitted(uint64_t submission) noexcept
    {
        mImpl->LastSubmittedSubmission = std::max(
            mImpl->LastSubmittedSubmission,
            submission);
        if (!mImpl->bFrameRecorded)
            return;
        mImpl->PreviousIndex = mImpl->CurrentIndex;
        mImpl->CurrentIndex = 1u - mImpl->PreviousIndex;
        mImpl->bFrameRecorded = false;
        mImpl->bResetThisFrame = false;
    }

    void SvgfDenoiser::AbortFrame() noexcept
    {
        mImpl->bFrameRecorded = false;
    }

    bool SvgfDenoiser::ConsumeGraphRebuildRequest() noexcept
    {
        return std::exchange(mImpl->bGraphRebuildRequested, false);
    }

    void SvgfDenoiser::Collect(uint64_t completedSubmission)
    {
        std::erase_if(mImpl->RetiredHistories,
            [completedSubmission](const Impl::RetiredHistory& retired)
            {
                return retired.Submission <= completedSubmission;
            });
    }

    void SvgfDenoiser::Clear(uint64_t completedSubmission)
    {
        Collect(completedSubmission);
        if (mImpl->History[0].Diffuse)
        {
            mImpl->RetiredHistories.push_back(Impl::RetiredHistory{
                .Submission = mImpl->LastSubmittedSubmission,
                .Sets = std::move(mImpl->History),
            });
        }
        mImpl->History = {};
        mImpl->Bindings.clear();
        mImpl->PreviousGraphHandles = {};
        mImpl->CurrentGraphHandles = {};
        mImpl->bGraphHandlesValid = false;
        mImpl->bResetPending = true;
        mImpl->bResetThisFrame = true;
        mImpl->bGraphRebuildRequested = true;
        Collect(completedSubmission);
    }
}
