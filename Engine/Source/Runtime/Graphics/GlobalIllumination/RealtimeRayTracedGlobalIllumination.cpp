#include "Hepch.h"

#include "Runtime/Graphics/GlobalIllumination/RealtimeRayTracedGlobalIllumination.h"

#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <glm/gtc/matrix_inverse.hpp>

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
        struct alignas(16) RealtimeRtgiConstants
        {
            glm::mat4 InverseViewProjection{ 1.0f };
            glm::vec4 CameraPositionAndMaxDistance{ 0.0f, 0.0f, 0.0f, 1000.0f };
            glm::uvec4 DimensionsFrameBounces{ 0u };
            glm::uvec4 SamplingSettings{ 0u };
            glm::vec4 EnvironmentRadiance{ 0.0f };
        };

        static_assert(sizeof(RealtimeRtgiConstants) == 128);

        struct RealtimeRtgiPassData
        {
            AccelerationStructureHandle SceneTlas;
            BufferHandle Vertices;
            BufferHandle Indices;
            BufferHandle Geometries;
            BufferHandle Materials;
            BufferHandle Instances;
            BufferHandle EmissiveTriangles;
            SceneSurfaceData Surface;
            TextureHandle NoisyDiffuse;
            TextureHandle RawDirectDiffuse;
            TextureHandle RawIndirectDiffuse;
            TextureHandle HitDistance;
            TextureHandle Confidence;
        };

        [[nodiscard]] std::string ReadShaderContentHash(
            const std::filesystem::path& path)
        {
            std::ifstream stream(path);
            if (!stream)
            {
                throw std::runtime_error(
                    "Failed to open real-time RTGI shader cache-key sidecar: " +
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
                "Real-time RTGI shader cache-key sidecar has no key: " +
                Utils::PathToUtf8(path));
        }

        struct PassBindingKey
        {
            std::array<nvrhi::ITexture*, 11> Textures{};

            [[nodiscard]] bool operator==(const PassBindingKey&) const = default;
        };

        struct PassBindingKeyHash
        {
            [[nodiscard]] size_t operator()(const PassBindingKey& key) const noexcept
            {
                size_t hash = 0;
                for (nvrhi::ITexture* texture : key.Textures)
                {
                    hash ^= reinterpret_cast<size_t>(texture) +
                        0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                }
                return hash;
            }
        };
    }

    struct RealtimeRayTracedGlobalIllumination::Impl
    {
        struct RetiredBindingSet
        {
            uint64_t Submission = 0;
            nvrhi::BindingSetHandle BindingSet;
        };

        explicit Impl(const RealtimeRtgiDesc& desc)
            : Device(desc.Device)
            , OwnedShaderManager(desc.SharedShaderManager
                ? nullptr
                : CreateScope<ShaderManager>(desc.Device))
            , ShaderManagerInstance(desc.SharedShaderManager
                ? *desc.SharedShaderManager
                : *OwnedShaderManager)
            , Scene(desc.Scene)
            , Denoiser(desc.Denoiser)
            , ShaderArtifactCacheRoot(desc.ShaderArtifactCacheRoot)
            , Backend(desc.Backend)
            , BinaryFormat(GetShaderBinaryFormat(desc.Backend))
        {
            if (!Device)
                throw std::invalid_argument("Realtime RTGI requires a valid NVRHI device");
            if (!Scene)
                throw std::invalid_argument("Realtime RTGI requires a RayTracingScene");
            if (ShaderArtifactCacheRoot.empty())
                throw std::invalid_argument("Realtime RTGI requires a shader artifact cache root");
            CreatePipeline();
        }

        void CreatePipeline()
        {
            const std::filesystem::path libraryPath =
                ShaderArtifactCacheRoot /
                std::string(GetShaderArtifactDirectoryName(BinaryFormat)) /
                "HardwareRayTracingRealtime__lib_6_5.lib";
            const ShaderLibraryCacheKey libraryKey{
                .Source = "RayTracing/HardwareRayTracingRealtime.hlsl",
                .Variant = "BASE",
                .TargetProfile = "lib_6_5",
                .ContentHash = ReadShaderContentHash(
                    MakeShaderCacheKeyPath(libraryPath)),
                .Backend = Backend,
                .BinaryFormat = BinaryFormat,
            };
            ShaderLibrary = ShaderManagerInstance.LoadShaderLibraryFile(
                libraryKey,
                libraryPath);
            RayGenerationShader = ShaderManagerInstance.GetShaderFromLibrary(
                libraryKey,
                "RealtimeRayGeneration",
                nvrhi::ShaderType::RayGeneration);
            MissShader = ShaderManagerInstance.GetShaderFromLibrary(
                libraryKey,
                "RealtimeMiss",
                nvrhi::ShaderType::Miss);
            ClosestHitShader = ShaderManagerInstance.GetShaderFromLibrary(
                libraryKey,
                "RealtimeClosestHit",
                nvrhi::ShaderType::ClosestHit);

            nvrhi::BindingLayoutDesc frameLayoutDesc;
            frameLayoutDesc
                .setVisibility(nvrhi::ShaderType::RayGeneration)
                .setRegisterSpaceAndDescriptorSet(0)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(
                    0,
                    sizeof(RealtimeRtgiConstants)));
            FrameBindingLayout = Device->createBindingLayout(frameLayoutDesc);
            nvrhi::BindingSetDesc frameSetDesc;
            frameSetDesc.addItem(nvrhi::BindingSetItem::PushConstants(
                0,
                sizeof(RealtimeRtgiConstants)));
            FrameBindingSet = Device->createBindingSet(
                frameSetDesc,
                FrameBindingLayout);

            nvrhi::BindingLayoutDesc sceneLayoutDesc;
            sceneLayoutDesc
                .setVisibility(nvrhi::ShaderType::AllRayTracing)
                .setRegisterSpaceAndDescriptorSet(2)
                .addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::RawBuffer_SRV(2))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6));
            SceneBindingLayout = Device->createBindingLayout(sceneLayoutDesc);

            nvrhi::BindingLayoutDesc passLayoutDesc;
            passLayoutDesc
                .setVisibility(nvrhi::ShaderType::RayGeneration)
                .setRegisterSpaceAndDescriptorSet(3);
            for (uint32_t slot = 0; slot < 6; ++slot)
                passLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot));
            for (uint32_t slot = 0; slot < 5; ++slot)
                passLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(slot));
            PassBindingLayout = Device->createBindingLayout(passLayoutDesc);

            if (!RayGenerationShader || !MissShader || !ClosestHitShader ||
                !FrameBindingLayout || !FrameBindingSet ||
                !SceneBindingLayout || !PassBindingLayout)
            {
                throw std::runtime_error("NVRHI failed to create real-time RTGI immutable state");
            }

            nvrhi::rt::PipelineDesc pipelineDesc;
            pipelineDesc
                .addShader(nvrhi::rt::PipelineShaderDesc()
                    .setExportName("RealtimeRayGeneration")
                    .setShader(RayGenerationShader))
                .addShader(nvrhi::rt::PipelineShaderDesc()
                    .setExportName("RealtimeMiss")
                    .setShader(MissShader))
                .addHitGroup(nvrhi::rt::PipelineHitGroupDesc()
                    .setExportName("RealtimeHitGroup")
                    .setClosestHitShader(ClosestHitShader))
                .addBindingLayout(FrameBindingLayout)
                .addBindingLayout(SceneBindingLayout)
                .addBindingLayout(PassBindingLayout)
                .setMaxPayloadSize(64)
                .setMaxAttributeSize(sizeof(float) * 2u)
                .setMaxRecursionDepth(1);
            Pipeline = Device->createRayTracingPipeline(pipelineDesc);
            if (!Pipeline)
                throw std::runtime_error("NVRHI failed to create the real-time RTGI pipeline");
            ShaderTable = Pipeline->createShaderTable(
                nvrhi::rt::ShaderTableDesc().enableCaching(3)
                    .setDebugName("Real-time RTGI Shader Table"));
            if (!ShaderTable)
                throw std::runtime_error("NVRHI failed to create the real-time RTGI shader table");
            ShaderTable->setRayGenerationShader("RealtimeRayGeneration");
            if (ShaderTable->addMissShader("RealtimeMiss") != 0 ||
                ShaderTable->addHitGroup("RealtimeHitGroup") != 0)
            {
                throw std::runtime_error("Real-time RTGI shader table indices are not deterministic");
            }
        }

        void UpdateSceneBindingSet()
        {
            const RayTracingSceneGpuResources& resources = Scene->GetGpuResources();
            if (SceneGeneration == resources.Generation && SceneBindingSet)
                return;
            if (SceneBindingSet)
            {
                RetiredSceneBindings.push_back(RetiredBindingSet{
                    .Submission = LastSubmittedSubmission,
                    .BindingSet = std::move(SceneBindingSet),
                });
            }
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(
                    0, resources.SceneTlas))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                    1, resources.Vertices))
                .addItem(nvrhi::BindingSetItem::RawBuffer_SRV(
                    2, resources.Indices))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                    3, resources.Geometries))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                    4, resources.Materials))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                    5, resources.Instances))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                    6, resources.EmissiveTriangles));
            SceneBindingSet = Device->createBindingSet(desc, SceneBindingLayout);
            if (!SceneBindingSet)
                throw std::runtime_error("NVRHI failed to bind real-time RTGI scene tables");
            SceneGeneration = resources.Generation;
        }

        [[nodiscard]] nvrhi::IBindingSet* GetPassBindingSet(
            const PassBindingKey& key)
        {
            const auto existing = PassBindingSets.find(key);
            if (existing != PassBindingSets.end())
                return existing->second;
            nvrhi::BindingSetDesc desc;
            for (uint32_t slot = 0; slot < 6; ++slot)
                desc.addItem(nvrhi::BindingSetItem::Texture_SRV(slot, key.Textures[slot]));
            for (uint32_t slot = 0; slot < 5; ++slot)
            {
                desc.addItem(nvrhi::BindingSetItem::Texture_UAV(
                    slot,
                    key.Textures[6u + slot]));
            }
            nvrhi::BindingSetHandle bindingSet = Device->createBindingSet(
                desc,
                PassBindingLayout);
            if (!bindingSet)
                throw std::runtime_error("NVRHI failed to create a real-time RTGI pass binding set");
            return PassBindingSets.emplace(key, std::move(bindingSet)).first->second;
        }

        nvrhi::DeviceHandle Device;
        Scope<ShaderManager> OwnedShaderManager;
        ShaderManager& ShaderManagerInstance;
        RayTracingScene* Scene = nullptr;
        IDenoiser* Denoiser = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
        ShaderBinaryFormat BinaryFormat = ShaderBinaryFormat::Dxil;
        nvrhi::ShaderLibraryHandle ShaderLibrary;
        nvrhi::ShaderHandle RayGenerationShader;
        nvrhi::ShaderHandle MissShader;
        nvrhi::ShaderHandle ClosestHitShader;
        nvrhi::BindingLayoutHandle FrameBindingLayout;
        nvrhi::BindingLayoutHandle SceneBindingLayout;
        nvrhi::BindingLayoutHandle PassBindingLayout;
        nvrhi::BindingSetHandle FrameBindingSet;
        nvrhi::BindingSetHandle SceneBindingSet;
        nvrhi::rt::PipelineHandle Pipeline;
        nvrhi::rt::ShaderTableHandle ShaderTable;
        std::unordered_map<
            PassBindingKey,
            nvrhi::BindingSetHandle,
            PassBindingKeyHash> PassBindingSets;
        std::vector<RetiredBindingSet> RetiredSceneBindings;
        RealtimeRtgiConstants Constants;
        GlobalIlluminationSettings Settings;
        GlobalIlluminationHistoryTracker HistoryTracker;
        GlobalIlluminationStats Stats{
            .Technique = GlobalIlluminationTechniqueKind::HardwareRayTracing,
            .Status = "Unprepared",
        };
        GlobalIlluminationOutputs LastRawOutputs;
        uint64_t SceneGeneration = 0;
        uint64_t LastSubmittedSubmission = 0;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t TraceWidth = 0;
        uint32_t TraceHeight = 0;
        bool bPrepared = false;
    };

    RealtimeRayTracedGlobalIllumination::RealtimeRayTracedGlobalIllumination(
        const RealtimeRtgiDesc& desc)
        : mImpl(CreateScope<Impl>(desc))
    {
    }

    RealtimeRayTracedGlobalIllumination::~RealtimeRayTracedGlobalIllumination() = default;

    std::function<void()> RealtimeRayTracedGlobalIllumination::CaptureGraphState()
    {
        return [impl = mImpl.get(), bindings = mImpl->PassBindingSets,
            outputs = mImpl->LastRawOutputs]() mutable noexcept
        {
            using std::swap;
            swap(impl->PassBindingSets, bindings); swap(impl->LastRawOutputs, outputs);
        };
    }

    void RealtimeRayTracedGlobalIllumination::ClearGraphResourceCaches() noexcept
    {
        mImpl->PassBindingSets.clear();
    }

    void RealtimeRayTracedGlobalIllumination::PrepareFrame(
        const RealtimeRtgiFrameParameters& frame,
        const GlobalIlluminationSettings& settings)
    {
        if (frame.Width == 0 || frame.Height == 0)
            throw std::invalid_argument("Real-time RTGI dimensions must be non-zero");
        if (!mImpl->Scene->IsReady())
            throw std::logic_error("Real-time RTGI requires a prepared RayTracingScene");
        if (settings.Technique !=
            GlobalIlluminationTechniqueKind::HardwareRayTracing)
        {
            throw std::invalid_argument("Real-time RTGI received the wrong technique setting");
        }
        if (settings.SamplesPerPixel != 1)
            throw std::invalid_argument("The real-time RTGI vertical slice requires exactly 1 spp");
        // Primary direct lighting is evaluated separately; one secondary hit
        // already provides indirect light and is valid for the shared Low preset.
        if (settings.MaxBounces < 1 || settings.MaxBounces > 4)
            throw std::invalid_argument("Real-time RTGI supports 1-4 diffuse bounces");
        if (!std::isfinite(settings.RenderScale) ||
            settings.RenderScale < 0.5f || settings.RenderScale > 1.0f)
        {
            throw std::invalid_argument("Real-time RTGI render scale must be in [0.5, 1.0]");
        }
        Collect(frame.CompletedSubmission);
        mImpl->UpdateSceneBindingSet();

        GlobalIlluminationHistorySignature signature = frame.HistorySignature;
        signature.Width = frame.Width;
        signature.Height = frame.Height;
        signature.RenderScale = settings.RenderScale;
        signature.Technique = settings.Technique;
        signature.Denoiser = settings.Denoiser;
        signature.SamplesPerPixel = settings.SamplesPerPixel;
        signature.MaxBounces = settings.MaxBounces;
        signature.bHalfResolution = settings.RenderScale < 0.999f;
        const GlobalIlluminationHistoryResetReason resetReasons =
            mImpl->HistoryTracker.Update(signature);
        if (resetReasons != GlobalIlluminationHistoryResetReason::None)
        {
            ++mImpl->Stats.HistoryResetCount;
            mImpl->Stats.LastResetReasons = resetReasons;
            if (mImpl->Denoiser)
                mImpl->Denoiser->ResetHistory(resetReasons);
        }
        if (mImpl->Denoiser)
            mImpl->Denoiser->PrepareFrame(signature, frame.CompletedSubmission);

        mImpl->Width = frame.Width;
        mImpl->Height = frame.Height;
        mImpl->TraceWidth = std::max(
            static_cast<uint32_t>(std::ceil(
                static_cast<float>(frame.Width) * settings.RenderScale)),
            1u);
        mImpl->TraceHeight = std::max(
            static_cast<uint32_t>(std::ceil(
                static_cast<float>(frame.Height) * settings.RenderScale)),
            1u);
        const float maximumDistance =
            std::isfinite(frame.MaximumRayDistance) && frame.MaximumRayDistance > 0.0f
            ? frame.MaximumRayDistance
            : std::max(frame.Camera.FarPlane, 1.0f);
        mImpl->Constants.InverseViewProjection = glm::inverse(
            frame.Camera.ViewProjection);
        mImpl->Constants.CameraPositionAndMaxDistance = glm::vec4(
            frame.Camera.Position,
            maximumDistance);
        mImpl->Constants.DimensionsFrameBounces = glm::uvec4(
            mImpl->TraceWidth,
            mImpl->TraceHeight,
            frame.FrameSequenceIndex,
            settings.MaxBounces);
        mImpl->Constants.SamplingSettings = glm::uvec4(
            frame.FixedSeed,
            mImpl->Scene->GetStats().EmissiveTriangleCount,
            0u,
            0u);
        mImpl->Constants.EnvironmentRadiance = glm::vec4(
            glm::max(frame.EnvironmentRadiance, glm::vec3(0.0f)),
            0.0f);
        mImpl->Settings = settings;
        mImpl->Stats.RaysPerPixel = 1;
        mImpl->Stats.BounceCount = settings.MaxBounces;
        mImpl->Stats.Status = "Ready";
        mImpl->bPrepared = true;
    }

    GlobalIlluminationPrepareResult
        RealtimeRayTracedGlobalIllumination::PrepareFrame(
            const GlobalIlluminationFrameContext& frame,
            const GlobalIlluminationSettings& settings)
    {
        PrepareFrame(
            RealtimeRtgiFrameParameters{
                .Camera = frame.Camera,
                .HistorySignature = frame.HistorySignature,
                .Width = frame.Width,
                .Height = frame.Height,
                .FrameSequenceIndex = frame.FrameSequenceIndex,
                .MaximumRayDistance = frame.MaximumRayDistance,
                .EnvironmentRadiance = frame.EnvironmentRadiance,
                .CompletedSubmission = frame.CompletedSubmission,
            },
            settings);
        return {
            .bGraphRebuildRequested = mImpl->Denoiser &&
                mImpl->Denoiser->ConsumeGraphRebuildRequest(),
        };
    }

    bool RealtimeRayTracedGlobalIllumination::IsSupported(
        const RenderCapabilities& capabilities) const
    {
        return QueryHardwareRayTracingSupport(capabilities).bRealtimeRtgi;
    }

    GlobalIlluminationOutputs RealtimeRayTracedGlobalIllumination::AddPasses(
        RenderGraph& renderGraph,
        const GlobalIlluminationInputs& inputs,
        const GlobalIlluminationSettings& settings)
    {
        if (!mImpl->bPrepared)
            throw std::logic_error("Real-time RTGI AddPasses requires PrepareFrame");
        if (!inputs.Surface || !inputs.SceneTlas ||
            !inputs.RayTracingVertexBuffer || !inputs.RayTracingIndexBuffer ||
            !inputs.GeometryBuffer || !inputs.MaterialBuffer ||
            !inputs.RayTracingInstanceBuffer || !inputs.LightBuffer)
        {
            throw std::invalid_argument("Real-time RTGI inputs are incomplete");
        }

        TextureHandle noisyDiffuse;
        TextureHandle rawDirectDiffuse;
        TextureHandle rawIndirectDiffuse;
        TextureHandle hitDistance;
        TextureHandle confidence;
        renderGraph.AddPass<RealtimeRtgiPassData>(
            "RTDirectAndIndirectTrace",
            RenderGraphQueue::Graphics,
            [inputs, settings, impl = mImpl.get(),
                &noisyDiffuse, &rawDirectDiffuse, &rawIndirectDiffuse,
                &hitDistance, &confidence](
                RenderGraphBuilder& builder,
                RealtimeRtgiPassData& data)
            {
                data.SceneTlas = builder.AccelerationStructureRead(inputs.SceneTlas);
                data.Vertices = builder.ShaderResource(inputs.RayTracingVertexBuffer);
                data.Indices = builder.ShaderResource(inputs.RayTracingIndexBuffer);
                data.Geometries = builder.ShaderResource(inputs.GeometryBuffer);
                data.Materials = builder.ShaderResource(inputs.MaterialBuffer);
                data.Instances = builder.ShaderResource(inputs.RayTracingInstanceBuffer);
                data.EmissiveTriangles = builder.ShaderResource(inputs.LightBuffer);
                data.Surface.Depth = builder.DepthRead(inputs.Surface.Depth);
                data.Surface.NormalRoughness = builder.ShaderResource(
                    inputs.Surface.NormalRoughness);
                data.Surface.BaseColorMetalness = builder.ShaderResource(
                    inputs.Surface.BaseColorMetalness);
                data.Surface.Emissive = builder.ShaderResource(inputs.Surface.Emissive);
                data.Surface.MotionVectors = builder.ShaderResource(
                    inputs.Surface.MotionVectors);
                data.Surface.MaterialId = builder.ShaderResource(inputs.Surface.MaterialId);
                data.NoisyDiffuse = builder.Write(builder.CreateTexture(
                    RenderGraphTextureDesc{
                        .DebugName = "RTGI Raw Diffuse",
                        .Width = impl->TraceWidth,
                        .Height = impl->TraceHeight,
                        .Format = nvrhi::Format::RGBA16_FLOAT,
                    }), RenderGraphUsage::UnorderedAccess);
                data.RawDirectDiffuse = builder.Write(builder.CreateTexture(
                    RenderGraphTextureDesc{
                        .DebugName = "RTGI Raw Direct Diffuse",
                        .Width = impl->TraceWidth,
                        .Height = impl->TraceHeight,
                        .Format = nvrhi::Format::RGBA16_FLOAT,
                    }), RenderGraphUsage::UnorderedAccess);
                data.RawIndirectDiffuse = builder.Write(builder.CreateTexture(
                    RenderGraphTextureDesc{
                        .DebugName = "RTGI Raw Indirect Diffuse",
                        .Width = impl->TraceWidth,
                        .Height = impl->TraceHeight,
                        .Format = nvrhi::Format::RGBA16_FLOAT,
                    }), RenderGraphUsage::UnorderedAccess);
                data.HitDistance = builder.Write(builder.CreateTexture(
                    RenderGraphTextureDesc{
                        .DebugName = "RTGI Hit Distance",
                        .Width = impl->TraceWidth,
                        .Height = impl->TraceHeight,
                        .Format = nvrhi::Format::R32_FLOAT,
                    }), RenderGraphUsage::UnorderedAccess);
                data.Confidence = builder.Write(builder.CreateTexture(
                    RenderGraphTextureDesc{
                        .DebugName = "RTGI Trace Confidence",
                        .Width = impl->TraceWidth,
                        .Height = impl->TraceHeight,
                        .Format = nvrhi::Format::R32_FLOAT,
                    }), RenderGraphUsage::UnorderedAccess);
                noisyDiffuse = data.NoisyDiffuse;
                rawDirectDiffuse = data.RawDirectDiffuse;
                rawIndirectDiffuse = data.RawIndirectDiffuse;
                hitDistance = data.HitDistance;
                confidence = data.Confidence;
                (void)settings;
            },
            [impl = mImpl.get()](
                RenderGraphContext& context,
                const RealtimeRtgiPassData& data)
            {
                (void)context.GetAccelerationStructure(
                    data.SceneTlas,
                    RenderGraphAccess::Read);
                (void)context.GetBuffer(data.Vertices, RenderGraphAccess::Read);
                (void)context.GetBuffer(data.Indices, RenderGraphAccess::Read);
                (void)context.GetBuffer(data.Geometries, RenderGraphAccess::Read);
                (void)context.GetBuffer(data.Materials, RenderGraphAccess::Read);
                (void)context.GetBuffer(data.Instances, RenderGraphAccess::Read);
                (void)context.GetBuffer(data.EmissiveTriangles, RenderGraphAccess::Read);
                nvrhi::ITexture* depth = context.GetTexture(
                    data.Surface.Depth,
                    RenderGraphAccess::Read);
                nvrhi::ITexture* normal = context.GetTexture(
                    data.Surface.NormalRoughness,
                    RenderGraphAccess::Read);
                nvrhi::ITexture* baseColor = context.GetTexture(
                    data.Surface.BaseColorMetalness,
                    RenderGraphAccess::Read);
                nvrhi::ITexture* emissive = context.GetTexture(
                    data.Surface.Emissive,
                    RenderGraphAccess::Read);
                nvrhi::ITexture* motion = context.GetTexture(
                    data.Surface.MotionVectors,
                    RenderGraphAccess::Read);
                nvrhi::ITexture* materialId = context.GetTexture(
                    data.Surface.MaterialId,
                    RenderGraphAccess::Read);
                nvrhi::ITexture* noisy = context.GetTexture(
                    data.NoisyDiffuse,
                    RenderGraphAccess::Write);
                nvrhi::ITexture* hitDistance = context.GetTexture(
                    data.HitDistance,
                    RenderGraphAccess::Write);
                nvrhi::ITexture* confidence = context.GetTexture(
                    data.Confidence,
                    RenderGraphAccess::Write);
                nvrhi::ITexture* rawDirect = context.GetTexture(
                    data.RawDirectDiffuse,
                    RenderGraphAccess::Write);
                nvrhi::ITexture* rawIndirect = context.GetTexture(
                    data.RawIndirectDiffuse,
                    RenderGraphAccess::Write);
                const PassBindingKey bindingKey{
                    .Textures = {
                        depth,
                        normal,
                        baseColor,
                        emissive,
                        motion,
                        materialId,
                        noisy,
                        rawDirect,
                        rawIndirect,
                        hitDistance,
                        confidence,
                    },
                };
                nvrhi::rt::State state;
                state.setShaderTable(impl->ShaderTable)
                    .addBindingSet(impl->FrameBindingSet)
                    .addBindingSet(impl->SceneBindingSet)
                    .addBindingSet(impl->GetPassBindingSet(bindingKey));
                nvrhi::ICommandList* commandList = context.GetCommandList();
                commandList->setRayTracingState(state);
                commandList->setPushConstants(
                    &impl->Constants,
                    sizeof(impl->Constants));
                commandList->dispatchRays(nvrhi::rt::DispatchRaysArguments()
                    .setDimensions(impl->TraceWidth, impl->TraceHeight));
            });

        GlobalIlluminationOutputs rawOutputs{
            .CompositeRadiance = noisyDiffuse,
            .DiffuseIndirect = rawIndirectDiffuse,
            .DirectDiffuse = rawDirectDiffuse,
            .RawDiffuse = noisyDiffuse,
            .RawDirectDiffuse = rawDirectDiffuse,
            .RawIndirectDiffuse = rawIndirectDiffuse,
            .HitDistance = hitDistance,
            .Confidence = confidence,
            .CoverageMask = confidence,
        };
        mImpl->LastRawOutputs = rawOutputs;
        if (!mImpl->Denoiser || settings.Denoiser == GlobalIlluminationDenoiserKind::None)
            return rawOutputs;
        GlobalIlluminationOutputs denoised = mImpl->Denoiser->AddPasses(
            renderGraph,
            GlobalIlluminationNoisySignals{
                .DiffuseIndirect = noisyDiffuse,
                .HitDistance = hitDistance,
            },
            GlobalIlluminationDenoiserGuides{
                .Surface = inputs.Surface,
                .Confidence = confidence,
                .Coverage = confidence,
            },
            settings);
        denoised.CompositeRadiance = denoised.DiffuseIndirect;
        denoised.DiffuseIndirect = rawIndirectDiffuse;
        denoised.DirectDiffuse = rawDirectDiffuse;
        denoised.RawDiffuse = noisyDiffuse;
        denoised.RawDirectDiffuse = rawDirectDiffuse;
        denoised.RawIndirectDiffuse = rawIndirectDiffuse;
        return denoised;
    }

    void RealtimeRayTracedGlobalIllumination::ResetHistory(
        GlobalIlluminationHistoryResetReason reason)
    {
        mImpl->HistoryTracker.Reset(reason);
        if (mImpl->Denoiser)
            mImpl->Denoiser->ResetHistory(reason);
    }

    bool RealtimeRayTracedGlobalIllumination::RequiresContinuousUpdate() const noexcept
    {
        return true;
    }

    GlobalIlluminationStats RealtimeRayTracedGlobalIllumination::GetStats() const
    {
        return mImpl->Stats;
    }

    const GlobalIlluminationOutputs&
        RealtimeRayTracedGlobalIllumination::GetLastRawOutputs() const noexcept
    {
        return mImpl->LastRawOutputs;
    }

    void RealtimeRayTracedGlobalIllumination::NotifyFrameSubmitted(
        uint64_t submission) noexcept
    {
        mImpl->LastSubmittedSubmission = std::max(
            mImpl->LastSubmittedSubmission,
            submission);
        if (mImpl->Denoiser)
            mImpl->Denoiser->NotifyFrameSubmitted(submission);
    }

    void RealtimeRayTracedGlobalIllumination::UpdateImportedResources(
        RenderGraph& renderGraph)
    {
        if (mImpl->Denoiser)
            mImpl->Denoiser->UpdateImportedResources(renderGraph);
    }

    void RealtimeRayTracedGlobalIllumination::AbortFrame() noexcept
    {
        if (mImpl->Denoiser)
            mImpl->Denoiser->AbortFrame();
    }

    void RealtimeRayTracedGlobalIllumination::Collect(
        uint64_t completedSubmission)
    {
        if (mImpl->Denoiser)
            mImpl->Denoiser->Collect(completedSubmission);
        std::erase_if(mImpl->RetiredSceneBindings,
            [completedSubmission](const Impl::RetiredBindingSet& retired)
            {
                return retired.Submission <= completedSubmission;
            });
    }

    void RealtimeRayTracedGlobalIllumination::Clear(
        uint64_t completedSubmission)
    {
        Collect(completedSubmission);
        if (mImpl->Denoiser)
            mImpl->Denoiser->Clear(completedSubmission);
        mImpl->PassBindingSets.clear();
        mImpl->SceneBindingSet = nullptr;
        mImpl->SceneGeneration = 0;
        mImpl->HistoryTracker.Reset(
            GlobalIlluminationHistoryResetReason::Explicit);
        mImpl->bPrepared = false;
    }
}
