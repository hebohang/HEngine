#include "Hepch.h"

#include "Runtime/Graphics/GlobalIllumination/ScreenSpaceGlobalIllumination.h"

#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Graphics/GlobalIllumination/GiDiagnosticReadback.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace HEngine
{
    namespace
    {
        // Match this shader's optional legacy table records exactly. Ordinary
        // raster SSGI never consumes a RayTracingScene or its filtered tables.
        constexpr uint32_t c_EmissiveTriangleStride = 6u * 16u;
        constexpr uint32_t c_GeometryRecordStride = 2u * 16u;
        struct alignas(16) ScreenSpaceTraceConstants
        {
            glm::uvec2 TraceDimensions{ 0u };
            glm::uvec2 FullDimensions{ 0u };
            uint32_t FrameSequenceIndex = 0;
            uint32_t RayCount = 1;
            uint32_t StepCount = 32;
            uint32_t HiZStartMip = 2;
            uint32_t DepthMipCount = 1;
            uint32_t BinaryRefinementSteps = 4;
            float MaximumRayDistance = 5.0f;
            float Thickness = 0.1f;
            float EdgeFade = 0.05f;
            float RayOriginOffset = 0.01f;
            uint32_t LightCount = 0;
            uint32_t MaxBounces = 1;
            uint32_t UseRasterSurface = 0;
            uint32_t UseMaterialAo = 0;
            uint32_t UseScreenAo = 0;
            uint32_t DiagnosticMode = 0;
            uint32_t CountDiagnostics = 0;
            uint32_t EnvironmentMisMode = 0;
            uint32_t CounterPadding[2]{};
            glm::uvec4 ReactivePixels{ 0u };
            uint32_t LocalReactiveBudget = 0;
            uint32_t ConfiguredRayCount = 1;
            uint32_t ReactivePadding[2]{};
        };

        static_assert(sizeof(ScreenSpaceTraceConstants) == 128);
        static_assert(offsetof(ScreenSpaceTraceConstants, ReactivePixels) == 96);
        static_assert(offsetof(ScreenSpaceTraceConstants, EnvironmentMisMode) == 84);

        struct ScreenSpacePassData
        {
            BufferHandle FrameConstants;
            BufferHandle DiagnosticCounters;
            BufferHandle LightBuffer;
            BufferHandle GeometryBuffer;
            TextureHandle Depth;
            TextureHandle DepthPyramid;
            TextureHandle NormalRoughness;
            TextureHandle BaseColorMetalness;
            TextureHandle MaterialId;
            TextureHandle OpaqueDirectHdr;
            TextureHandle Emissive;
            TextureHandle Environment;
            TextureHandle EnvironmentCdf;
            std::shared_ptr<const EnvironmentSamplingProposal> EnvironmentProposal;
            TextureHandle PackedNormalRoughnessAo;
            TextureHandle AmbientOcclusion;
            bool bRasterSurface = false;
            bool bMaterialAo = false;
            bool bScreenAo = false;
            TextureHandle RawDiffuse;
            TextureHandle HitDistance;
            TextureHandle Confidence;
            TextureHandle Coverage;
            TextureHandle RawDirectDiffuse;
            TextureHandle RawIndirectDiffuse;
        };

        struct BindingKey
        {
            std::array<nvrhi::ITexture*, 17> Textures{};
            nvrhi::IBuffer* LightBuffer = nullptr;
            nvrhi::IBuffer* GeometryBuffer = nullptr;

            [[nodiscard]] bool operator==(const BindingKey&) const = default;
        };

        struct BindingKeyHash
        {
            [[nodiscard]] size_t operator()(const BindingKey& key) const noexcept
            {
                size_t hash = 0;
                for (nvrhi::ITexture* texture : key.Textures)
                {
                    hash ^= reinterpret_cast<size_t>(texture) +
                        0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                }
                hash ^= reinterpret_cast<size_t>(key.LightBuffer) +
                    0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                hash ^= reinterpret_cast<size_t>(key.GeometryBuffer) +
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
                    "Failed to open SSGI shader cache-key sidecar: " +
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
                "SSGI shader cache-key sidecar has no key: " +
                Utils::PathToUtf8(path));
        }

        [[nodiscard]] uint32_t DispatchCount(uint32_t value) noexcept
        {
            return (value + 7u) / 8u;
        }
    }

    struct ScreenSpaceGlobalIllumination::Impl
    {
        explicit Impl(const ScreenSpaceGlobalIlluminationDesc& desc)
            : Device(desc.Device)
            , Diagnostics(desc.Device, "SSGI")
            , OwnedShaderManager(desc.SharedShaderManager
                ? nullptr
                : CreateScope<ShaderManager>(desc.Device))
            , ShaderManagerInstance(desc.SharedShaderManager
                ? *desc.SharedShaderManager
                : *OwnedShaderManager)
            , Denoiser(desc.Denoiser)
            , ShaderArtifactCacheRoot(desc.ShaderArtifactCacheRoot)
            , Backend(desc.Backend)
            , BinaryFormat(GetShaderBinaryFormat(desc.Backend))
            , FramesInFlight(std::max(desc.FramesInFlight, 1u))
        {
            if (!Device)
                throw std::invalid_argument("SSGI requires a valid NVRHI device");
            if (ShaderArtifactCacheRoot.empty())
                throw std::invalid_argument("SSGI requires a shader artifact cache root");
            CreateImmutableState();
        }

        void CreateImmutableState()
        {
            const std::filesystem::path shaderPath =
                ShaderArtifactCacheRoot /
                std::string(GetShaderArtifactDirectoryName(BinaryFormat)) /
                "ScreenSpaceGlobalIllumination.cs";
            Shader = ShaderManagerInstance.LoadShaderFile(
                ShaderCacheKey{
                    .Source = "GlobalIllumination/ScreenSpaceGlobalIllumination.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Compute,
                    .EntryPoint = "CS",
                    .TargetProfile = "cs_6_0",
                    .ContentHash = ReadShaderContentHash(
                        MakeShaderCacheKeyPath(shaderPath)),
                    .Backend = Backend,
                    .BinaryFormat = BinaryFormat,
                },
                shaderPath,
                "SSGI Ray March");

            nvrhi::BindingLayoutDesc frameLayoutDesc;
            frameLayoutDesc.setVisibility(nvrhi::ShaderType::Compute)
                .setRegisterSpaceAndDescriptorSet(0)
                .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
            FrameBindingLayout = Device->createBindingLayout(frameLayoutDesc);

            nvrhi::BindingLayoutDesc passLayoutDesc;
            passLayoutDesc.setVisibility(nvrhi::ShaderType::Compute)
                .setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(
                    0,
                    sizeof(ScreenSpaceTraceConstants)));
            for (uint32_t slot = 0; slot < 6; ++slot)
                passLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot));
            passLayoutDesc.addItem(
                nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6));
            passLayoutDesc.addItem(
                nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7));
            passLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(8));
            passLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(9));
            passLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(10));
            passLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(11));
            passLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(12));
            passLayoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
            for (uint32_t slot = 0; slot < 6; ++slot)
                passLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(slot));
            passLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(6));
            PassBindingLayout = Device->createBindingLayout(passLayoutDesc);
            EnvironmentSampler = Device->createSampler(nvrhi::SamplerDesc()
                .setAllFilters(true)
                .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));

            nvrhi::ComputePipelineDesc pipelineDesc;
            pipelineDesc.setComputeShader(Shader)
                .addBindingLayout(FrameBindingLayout)
                .addBindingLayout(PassBindingLayout);
            Pipeline = Device->createComputePipeline(pipelineDesc);
            if (!Shader || !FrameBindingLayout || !PassBindingLayout || !Pipeline ||
                !EnvironmentSampler)
            {
                throw std::runtime_error(
                    "NVRHI failed to create SSGI immutable state");
            }

            EmptyEmissiveTriangles = Device->createBuffer(nvrhi::BufferDesc()
                .setByteSize(c_EmissiveTriangleStride)
                .setStructStride(c_EmissiveTriangleStride)
                .setInitialState(nvrhi::ResourceStates::Common)
                .setDebugName("SSGI Empty Emissive Triangles"));
            EmptyGeometries = Device->createBuffer(nvrhi::BufferDesc()
                .setByteSize(c_GeometryRecordStride)
                .setStructStride(c_GeometryRecordStride)
                .setInitialState(nvrhi::ResourceStates::Common)
                .setDebugName("SSGI Empty Geometry Records"));
            if (!EmptyEmissiveTriangles || !EmptyGeometries)
                throw std::runtime_error("NVRHI failed to create SSGI empty table resources");
            nvrhi::CommandListHandle initialize = Device->createCommandList();
            if (!initialize)
                throw std::runtime_error("NVRHI failed to create SSGI table initialization command list");
            constexpr std::array<uint32_t, c_EmissiveTriangleStride / sizeof(uint32_t)> zero{};
            initialize->open();
            // Newly allocated default-heap buffers start in Common. A desc's
            // initialState alone does not establish command-list tracking when
            // keepInitialState is false; establish it before writeBuffer asks
            // for CopyDest, then permanently transition the initialized data.
            initialize->beginTrackingBufferState(EmptyEmissiveTriangles, nvrhi::ResourceStates::Common);
            initialize->beginTrackingBufferState(EmptyGeometries, nvrhi::ResourceStates::Common);
            initialize->writeBuffer(EmptyEmissiveTriangles, zero.data(), c_EmissiveTriangleStride);
            initialize->writeBuffer(EmptyGeometries, zero.data(), c_GeometryRecordStride);
            initialize->setPermanentBufferState(EmptyEmissiveTriangles, nvrhi::ResourceStates::ShaderResource);
            initialize->setPermanentBufferState(EmptyGeometries, nvrhi::ResourceStates::ShaderResource);
            initialize->commitBarriers();
            initialize->close();
            // The graph currently submits on this same graphics queue. Queue
            // order makes this one-time 128-byte upload visible without a CPU
            // wait or any per-frame table construction/upload.
            Device->executeCommandList(initialize, nvrhi::CommandQueue::Graphics);
        }

        [[nodiscard]] nvrhi::IBindingSet* GetBindingSet(const BindingKey& key)
        {
            const auto existing = BindingSets.find(key);
            if (existing != BindingSets.end())
                return existing->second;
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::PushConstants(
                0,
                sizeof(ScreenSpaceTraceConstants)));
            for (uint32_t slot = 0; slot < 6; ++slot)
                desc.addItem(nvrhi::BindingSetItem::Texture_SRV(
                    slot,
                    key.Textures[slot]));
            desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                6,
                key.LightBuffer));
            desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                7,
                key.GeometryBuffer));
            desc.addItem(nvrhi::BindingSetItem::Texture_SRV(8, key.Textures[12]));
            desc.addItem(nvrhi::BindingSetItem::Texture_SRV(9, key.Textures[13]));
            desc.addItem(nvrhi::BindingSetItem::Texture_SRV(10, key.Textures[14]));
            desc.addItem(nvrhi::BindingSetItem::Texture_SRV(11, key.Textures[15]));
            desc.addItem(nvrhi::BindingSetItem::Texture_SRV(12, key.Textures[16]));
            desc.addItem(nvrhi::BindingSetItem::Sampler(0, EnvironmentSampler));
            for (uint32_t slot = 0; slot < 6; ++slot)
                desc.addItem(nvrhi::BindingSetItem::Texture_UAV(
                    slot,
                    key.Textures[6u + slot]));
            desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(6, Diagnostics.GetBuffer()));
            nvrhi::BindingSetHandle bindingSet = Device->createBindingSet(
                desc,
                PassBindingLayout);
            if (!bindingSet)
                throw std::runtime_error("NVRHI failed to bind SSGI pass resources");
            return BindingSets.emplace(key, std::move(bindingSet)).first->second;
        }

        [[nodiscard]] nvrhi::IBindingSet* GetFrameBindingSet(
            nvrhi::IBuffer* frameConstants)
        {
            if (FrameBindingSet && FrameBindingBuffer == frameConstants)
                return FrameBindingSet;
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::ConstantBuffer(
                0,
                frameConstants));
            FrameBindingSet = Device->createBindingSet(desc, FrameBindingLayout);
            if (!FrameBindingSet)
                throw std::runtime_error("NVRHI failed to bind SSGI frame constants");
            FrameBindingBuffer = frameConstants;
            return FrameBindingSet;
        }

        nvrhi::DeviceHandle Device;
        GiDiagnosticReadback Diagnostics;
        Scope<ShaderManager> OwnedShaderManager;
        ShaderManager& ShaderManagerInstance;
        IDenoiser* Denoiser = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
        ShaderBinaryFormat BinaryFormat = ShaderBinaryFormat::Dxil;
        uint32_t FramesInFlight = 1;
        nvrhi::ShaderHandle Shader;
        nvrhi::BindingLayoutHandle FrameBindingLayout;
        nvrhi::BindingLayoutHandle PassBindingLayout;
        nvrhi::BindingSetHandle FrameBindingSet;
        nvrhi::IBuffer* FrameBindingBuffer = nullptr;
        nvrhi::ComputePipelineHandle Pipeline;
        nvrhi::SamplerHandle EnvironmentSampler;
        nvrhi::BufferHandle EmptyEmissiveTriangles;
        nvrhi::BufferHandle EmptyGeometries;
        std::unordered_map<BindingKey, nvrhi::BindingSetHandle, BindingKeyHash>
            BindingSets;
        ScreenSpaceTraceConstants TraceConstants;
        GlobalIlluminationSettings Settings;
        GlobalIlluminationHistoryTracker HistoryTracker;
        GlobalIlluminationOutputs LastRawOutputs;
        GlobalIlluminationStats Stats{
            .Technique = GlobalIlluminationTechniqueKind::ScreenSpace,
            .Status = "Unprepared",
        };
        uint64_t LastSubmittedSubmission = 0;
        uint32_t FullWidth = 0;
        uint32_t FullHeight = 0;
        uint32_t TraceWidth = 0;
        uint32_t TraceHeight = 0;
        bool bPrepared = false;
    };

    ScreenSpaceGlobalIllumination::ScreenSpaceGlobalIllumination(
        const ScreenSpaceGlobalIlluminationDesc& desc)
        : mImpl(CreateScope<Impl>(desc))
    {
    }

    ScreenSpaceGlobalIllumination::~ScreenSpaceGlobalIllumination() = default;

    std::function<void()> ScreenSpaceGlobalIllumination::CaptureGraphState()
    {
        return [impl = mImpl.get(), bindings = mImpl->BindingSets,
            outputs = mImpl->LastRawOutputs,
            pixels = mImpl->TraceConstants.ReactivePixels,
            local = mImpl->TraceConstants.LocalReactiveBudget,
            configured = mImpl->TraceConstants.ConfiguredRayCount]() mutable noexcept
        {
            using std::swap;
            swap(impl->BindingSets, bindings); swap(impl->LastRawOutputs, outputs);
            swap(impl->TraceConstants.ReactivePixels, pixels);
            swap(impl->TraceConstants.LocalReactiveBudget, local);
            swap(impl->TraceConstants.ConfiguredRayCount, configured);
        };
    }

    void ScreenSpaceGlobalIllumination::ClearGraphResourceCaches() noexcept
    {
        mImpl->BindingSets.clear();
    }

    void ScreenSpaceGlobalIllumination::PrepareFrame(
        const ScreenSpaceGlobalIlluminationFrameParameters& frame,
        const GlobalIlluminationSettings& settings)
    {
        if (frame.Width == 0 || frame.Height == 0)
            throw std::invalid_argument("SSGI dimensions must be non-zero");
        if (settings.Technique != GlobalIlluminationTechniqueKind::ScreenSpace)
            throw std::invalid_argument("SSGI received the wrong technique setting");
        if (!std::isfinite(settings.RenderScale) || settings.RenderScale < 0.5f ||
            settings.RenderScale > 1.0f)
        {
            throw std::invalid_argument("SSGI render scale must be in [0.5, 1.0]");
        }
        if (settings.ScreenSpaceRayCount < 1 ||
            settings.ScreenSpaceRayCount > 8 ||
            settings.ScreenSpaceStepCount < 1 ||
            settings.ScreenSpaceStepCount > 256 ||
            settings.ScreenSpaceBinaryRefinementSteps > 16 ||
            !std::isfinite(settings.ScreenSpaceMaximumRayDistance) ||
            settings.ScreenSpaceMaximumRayDistance <= 0.0f ||
            !std::isfinite(settings.ScreenSpaceThickness) ||
            settings.ScreenSpaceThickness <= 0.0f ||
            !std::isfinite(settings.ScreenSpaceEdgeFade) ||
            settings.ScreenSpaceEdgeFade < 0.0f ||
            settings.ScreenSpaceEdgeFade > 0.5f)
        {
            throw std::invalid_argument("SSGI ray-march settings are outside supported bounds");
        }

        Collect(frame.CompletedSubmission);
        const uint32_t effectiveBounceCount =
            std::clamp(settings.MaxBounces, 1u, 4u);
        GlobalIlluminationHistorySignature signature = frame.HistorySignature;
        signature.Width = frame.Width;
        signature.Height = frame.Height;
        signature.RenderScale = settings.RenderScale;
        signature.Technique = settings.Technique;
        signature.Denoiser = settings.Denoiser;
        signature.SamplesPerPixel = settings.ScreenSpaceRayCount;
        signature.MaxBounces = effectiveBounceCount;
        signature.bHalfResolution = settings.RenderScale < 0.999f;
        // Sampling policy changes must not silently reuse the previous estimator history.
        if (settings.bSsgiEnvironmentMis) signature.TechniqueStateFingerprint ^= 0x454e564d495331ull;
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
        {
            mImpl->Denoiser->SetReactiveRegion(settings.bSsgiLocalReactiveBudget ? frame.ReactiveRegion : SsgiReactiveRegion{});
            mImpl->Denoiser->PrepareFrame(signature, frame.CompletedSubmission);
        }
        const SsgiReactiveRegion reactive = mImpl->Denoiser ? mImpl->Denoiser->GetReactiveRegion() : SsgiReactiveRegion{};
        mImpl->Diagnostics.Prepare(frame.CompletedSubmission);
        const uint32_t actualRayCount = mImpl->Denoiser && settings.Denoiser == GlobalIlluminationDenoiserKind::Svgf
            ? std::max(settings.ScreenSpaceRayCount, mImpl->Denoiser->GetMinimumRayCount())
            : settings.ScreenSpaceRayCount;

        mImpl->FullWidth = frame.Width;
        mImpl->FullHeight = frame.Height;
        mImpl->TraceWidth = std::max(
            static_cast<uint32_t>(std::ceil(frame.Width * settings.RenderScale)),
            1u);
        mImpl->TraceHeight = std::max(
            static_cast<uint32_t>(std::ceil(frame.Height * settings.RenderScale)),
            1u);
        mImpl->TraceConstants = {
            .TraceDimensions = { mImpl->TraceWidth, mImpl->TraceHeight },
            .FullDimensions = { frame.Width, frame.Height },
            .FrameSequenceIndex = frame.FrameSequenceIndex,
            .RayCount = actualRayCount,
            .StepCount = settings.ScreenSpaceStepCount,
            .HiZStartMip = settings.ScreenSpaceHiZStartMip,
            .DepthMipCount = std::max(frame.DepthMipCount, 1u),
            .BinaryRefinementSteps = settings.ScreenSpaceBinaryRefinementSteps,
            .MaximumRayDistance = settings.ScreenSpaceMaximumRayDistance,
            .Thickness = settings.ScreenSpaceThickness,
            .EdgeFade = settings.ScreenSpaceEdgeFade,
            .RayOriginOffset = std::max(
                settings.ScreenSpaceThickness * 0.1f,
                1e-4f),
            .LightCount = frame.EmissiveTriangleCount,
            .MaxBounces = effectiveBounceCount,
            .DiagnosticMode = settings.DebugView == GlobalIlluminationDebugView::TraceHitLocation ? 2u :
                (settings.DebugView == GlobalIlluminationDebugView::TraceTermination ? 1u : 0u),
        };
        mImpl->Stats.ReactiveRegion = reactive;
        mImpl->TraceConstants.ReactivePixels = reactive.Pixels;
        mImpl->TraceConstants.LocalReactiveBudget = reactive.bEnabled ? 1u : 0u;
        mImpl->TraceConstants.ConfiguredRayCount = settings.ScreenSpaceRayCount;
        mImpl->Settings = settings;
        mImpl->Stats.RaysPerPixel = actualRayCount;
        mImpl->Stats.bScreenSpaceBudgetValid = true;
        mImpl->Stats.ConfiguredScreenSpaceRays = settings.ScreenSpaceRayCount;
        mImpl->Stats.SelectedScreenSpaceBudgetRays = actualRayCount;
        mImpl->Stats.ScreenSpaceTraceWidth = mImpl->TraceWidth;
        mImpl->Stats.ScreenSpaceTraceHeight = mImpl->TraceHeight;
        mImpl->Stats.DenoiserBudget = mImpl->Denoiser &&
            settings.Denoiser == GlobalIlluminationDenoiserKind::Svgf
            ? mImpl->Denoiser->GetBudgetDiagnostics() : DenoiserBudgetDiagnostics{};
        mImpl->Stats.FrameSequenceIndex = frame.FrameSequenceIndex;
        mImpl->Stats.BounceCount = effectiveBounceCount;
        mImpl->Stats.Status = "Ready";
        mImpl->bPrepared = true;
    }

    GlobalIlluminationPrepareResult
        ScreenSpaceGlobalIllumination::PrepareFrame(
            const GlobalIlluminationFrameContext& frame,
            const GlobalIlluminationSettings& settings)
    {
        PrepareFrame(
            ScreenSpaceGlobalIlluminationFrameParameters{
                .Camera = frame.Camera,
                .HistorySignature = frame.HistorySignature,
                .ReactiveRegion = frame.ReactiveRegion,
                .Width = frame.Width,
                .Height = frame.Height,
                .FrameSequenceIndex = frame.FrameSequenceIndex,
                .DepthMipCount = frame.DepthMipCount,
                .EmissiveTriangleCount = frame.EmissiveTriangleCount,
                .CompletedSubmission = frame.CompletedSubmission,
            },
            settings);
        return {
            .bGraphRebuildRequested = mImpl->Denoiser &&
                mImpl->Denoiser->ConsumeGraphRebuildRequest(),
        };
    }

    bool ScreenSpaceGlobalIllumination::IsSupported(
        const RenderCapabilities& capabilities) const
    {
        return capabilities.BaselineRaster.bEnabled;
    }

    GlobalIlluminationOutputs ScreenSpaceGlobalIllumination::AddPasses(
        RenderGraph& renderGraph,
        const GlobalIlluminationInputs& inputs,
        const GlobalIlluminationSettings& settings)
    {
        if (!mImpl->bPrepared)
            throw std::logic_error("SSGI AddPasses requires PrepareFrame");
        if (!inputs.Surface)
            throw std::invalid_argument("SSGI surface guides are incomplete");
        if (!inputs.DepthPyramid)
            throw std::invalid_argument("SSGI depth pyramid is missing");
        if (!inputs.OpaqueDirectHdr)
            throw std::invalid_argument("SSGI pre-GI opaque HDR is missing");
        if (!inputs.FrameBuffer)
            throw std::invalid_argument("SSGI frame constants are missing");
        if (!inputs.Environment)
            throw std::invalid_argument("SSGI scene environment cube is missing");
        if (inputs.Surface.bMaterialIdIsRayTracingIndex &&
            (!inputs.LightBuffer || !inputs.GeometryBuffer))
            throw std::invalid_argument("Legacy RT-index SSGI surface requires matching emissive/geometry tables");

        TextureHandle rawDiffuse;
        TextureHandle hitDistance;
        TextureHandle confidence;
        TextureHandle coverage;
        TextureHandle rawDirectDiffuse;
        TextureHandle rawIndirectDiffuse;
        const auto diagnosticCounters = mImpl->Diagnostics.AddReset(renderGraph, "SSGI");
        renderGraph.AddPass<ScreenSpacePassData>(
            "SSGIRayMarch",
            RenderGraphQueue::Compute,
            [impl = mImpl.get(), inputs, diagnosticCounters, &rawDiffuse, &hitDistance,
                &confidence, &coverage, &rawDirectDiffuse,
                &rawIndirectDiffuse](
                RenderGraphBuilder& builder,
                ScreenSpacePassData& data)
            {
                data.DiagnosticCounters = builder.UnorderedAccess(diagnosticCounters);
                data.FrameConstants = builder.ConstantBuffer(inputs.FrameBuffer);
                if (inputs.Surface.bMaterialIdIsRayTracingIndex)
                {
                    data.LightBuffer = builder.ShaderResource(inputs.LightBuffer);
                    data.GeometryBuffer = builder.ShaderResource(inputs.GeometryBuffer);
                }
                data.Depth = builder.DepthRead(inputs.Surface.Depth);
                data.DepthPyramid = builder.ShaderResource(inputs.DepthPyramid);
                data.NormalRoughness = builder.ShaderResource(
                    inputs.Surface.NormalRoughness);
                data.BaseColorMetalness = builder.ShaderResource(
                    inputs.Surface.BaseColorMetalness);
                data.MaterialId = builder.ShaderResource(
                    inputs.Surface.MaterialId);
                data.OpaqueDirectHdr = builder.ShaderResource(
                    inputs.OpaqueDirectHdr);
                data.Emissive = builder.ShaderResource(inputs.Surface.Emissive);
                data.Environment = builder.ShaderResource(inputs.Environment);
                data.EnvironmentProposal = inputs.EnvironmentCdf ? inputs.EnvironmentProposal : nullptr;
                // Disabled/fallback MIS never reads this binding. Reuse a color
                // SRV; depth is already declared with depth-read layout here.
                data.EnvironmentCdf = builder.ShaderResource(inputs.EnvironmentCdf
                    ? inputs.EnvironmentCdf : inputs.Surface.NormalRoughness);
                data.bMaterialAo = bool(inputs.PackedNormalRoughnessAo);
                data.bScreenAo = bool(inputs.AmbientOcclusion);
                data.PackedNormalRoughnessAo = builder.ShaderResource(
                    data.bMaterialAo ? inputs.PackedNormalRoughnessAo :
                        inputs.Surface.NormalRoughness);
                data.AmbientOcclusion = builder.ShaderResource(
                    data.bScreenAo ? inputs.AmbientOcclusion :
                        inputs.Surface.NormalRoughness);
                data.bRasterSurface = !inputs.Surface.bMaterialIdIsRayTracingIndex;
                const auto makeOutput = [&builder, impl](
                    std::string debugName,
                    nvrhi::Format format)
                {
                    return builder.Write(builder.CreateTexture(
                        RenderGraphTextureDesc{
                            .DebugName = std::move(debugName),
                            .Width = impl->TraceWidth,
                            .Height = impl->TraceHeight,
                            .Format = format,
                        }), RenderGraphUsage::UnorderedAccess);
                };
                data.RawDiffuse = makeOutput(
                    "SSGI Raw Diffuse",
                    nvrhi::Format::RGBA16_FLOAT);
                data.HitDistance = makeOutput(
                    "SSGI Hit Distance",
                    nvrhi::Format::R32_FLOAT);
                data.Confidence = makeOutput(
                    "SSGI Confidence",
                    nvrhi::Format::R32_FLOAT);
                data.Coverage = makeOutput(
                    "SSGI Coverage",
                    nvrhi::Format::R32_FLOAT);
                data.RawDirectDiffuse = makeOutput(
                    "SSGI Raw Direct Diffuse",
                    nvrhi::Format::RGBA16_FLOAT);
                data.RawIndirectDiffuse = makeOutput(
                    "SSGI Raw Indirect Diffuse",
                    nvrhi::Format::RGBA16_FLOAT);
                rawDiffuse = data.RawDiffuse;
                hitDistance = data.HitDistance;
                confidence = data.Confidence;
                coverage = data.Coverage;
                rawDirectDiffuse = data.RawDirectDiffuse;
                rawIndirectDiffuse = data.RawIndirectDiffuse;
            },
            [impl = mImpl.get()](
                RenderGraphContext& context,
                const ScreenSpacePassData& data)
            {
                nvrhi::IBuffer* frameConstants = context.GetBuffer(
                    data.FrameConstants,
                    RenderGraphAccess::Read);
                nvrhi::IBuffer* lightBuffer = data.bRasterSurface
                    ? impl->EmptyEmissiveTriangles.Get()
                    : context.GetBuffer(data.LightBuffer, RenderGraphAccess::Read);
                nvrhi::IBuffer* geometryBuffer = data.bRasterSurface
                    ? impl->EmptyGeometries.Get()
                    : context.GetBuffer(data.GeometryBuffer, RenderGraphAccess::Read);
                if (lightBuffer->getDesc().structStride != c_EmissiveTriangleStride ||
                    geometryBuffer->getDesc().structStride != c_GeometryRecordStride)
                    throw std::runtime_error("SSGI legacy table structured strides do not match shader records");
                const BindingKey key{
                    .Textures = {
                        context.GetTexture(data.Depth, RenderGraphAccess::Read),
                        context.GetTexture(
                            data.DepthPyramid,
                            RenderGraphAccess::Read),
                        context.GetTexture(
                            data.NormalRoughness,
                            RenderGraphAccess::Read),
                        context.GetTexture(
                            data.BaseColorMetalness,
                            RenderGraphAccess::Read),
                        context.GetTexture(
                            data.MaterialId,
                            RenderGraphAccess::Read),
                        context.GetTexture(
                            data.OpaqueDirectHdr,
                            RenderGraphAccess::Read),
                        context.GetTexture(data.RawDiffuse, RenderGraphAccess::Write),
                        context.GetTexture(data.HitDistance, RenderGraphAccess::Write),
                        context.GetTexture(data.Confidence, RenderGraphAccess::Write),
                        context.GetTexture(data.Coverage, RenderGraphAccess::Write),
                        context.GetTexture(
                            data.RawDirectDiffuse,
                            RenderGraphAccess::Write),
                        context.GetTexture(
                            data.RawIndirectDiffuse,
                            RenderGraphAccess::Write),
                        context.GetTexture(data.Emissive, RenderGraphAccess::Read),
                        context.GetTexture(data.Environment, RenderGraphAccess::Read),
                        context.GetTexture(data.PackedNormalRoughnessAo, RenderGraphAccess::Read),
                        context.GetTexture(data.AmbientOcclusion, RenderGraphAccess::Read),
                        context.GetTexture(data.EnvironmentCdf, RenderGraphAccess::Read),
                    },
                    .LightBuffer = lightBuffer,
                    .GeometryBuffer = geometryBuffer,
                };
                nvrhi::ICommandList* commandList = context.GetCommandList();
                nvrhi::ComputeState state;
                state.setPipeline(impl->Pipeline)
                    .addBindingSet(impl->GetFrameBindingSet(frameConstants))
                    .addBindingSet(impl->GetBindingSet(key));
                commandList->setComputeState(state);
                (void)context.GetBuffer(data.DiagnosticCounters, RenderGraphAccess::ReadWrite);
                ScreenSpaceTraceConstants traceConstants = impl->TraceConstants;
                traceConstants.CountDiagnostics = impl->Diagnostics.IsRecording() ? 1u : 0u;
                traceConstants.UseRasterSurface = data.bRasterSurface ? 1u : 0u;
                if (data.bRasterSurface)
                    traceConstants.LightCount = 0u;
                traceConstants.UseMaterialAo = data.bMaterialAo ? 1u : 0u;
                traceConstants.UseScreenAo = data.bScreenAo ? 1u : 0u;
                // Evaluate the actual selected budget each recording; a history reset
                // may temporarily select eight rays without rebuilding this graph.
                traceConstants.EnvironmentMisMode = !impl->Settings.bSsgiEnvironmentMis ? 0u :
                    !data.bRasterSurface ? 3u : traceConstants.MaxBounces != 1u ? 4u :
                    (traceConstants.RayCount < 2u || (traceConstants.RayCount & 1u) != 0u) ? 5u :
                    !data.EnvironmentProposal ? 2u : 1u;
                impl->Stats.EnvironmentMisMode = traceConstants.EnvironmentMisMode;
                commandList->setPushConstants(&traceConstants, sizeof(traceConstants));
                commandList->dispatch(
                    DispatchCount(impl->TraceWidth),
                    DispatchCount(impl->TraceHeight));
            });

        mImpl->Diagnostics.AddReadback(renderGraph, diagnosticCounters, "SSGI");

        GlobalIlluminationOutputs rawOutputs{
            .CompositeRadiance = rawDiffuse,
            .DiffuseIndirect = rawIndirectDiffuse,
            .DirectDiffuse = rawDirectDiffuse,
            .RawDiffuse = rawDiffuse,
            .RawDirectDiffuse = rawDirectDiffuse,
            .RawIndirectDiffuse = rawIndirectDiffuse,
            .HitDistance = hitDistance,
            .Confidence = confidence,
            .CoverageMask = coverage,
        };
        mImpl->LastRawOutputs = rawOutputs;
        if (!mImpl->Denoiser || settings.Denoiser ==
            GlobalIlluminationDenoiserKind::None)
        {
            return rawOutputs;
        }
        GlobalIlluminationOutputs denoised = mImpl->Denoiser->AddPasses(
            renderGraph,
            GlobalIlluminationNoisySignals{
                .DiffuseIndirect = rawDiffuse,
                .HitDistance = hitDistance,
            },
            GlobalIlluminationDenoiserGuides{
                .Surface = inputs.Surface,
                .Confidence = confidence,
                .Coverage = coverage,
            },
            settings);
        denoised.CompositeRadiance = denoised.DiffuseIndirect;
        denoised.DiffuseIndirect = rawIndirectDiffuse;
        denoised.DirectDiffuse = rawDirectDiffuse;
        denoised.RawDiffuse = rawDiffuse;
        denoised.RawDirectDiffuse = rawDirectDiffuse;
        denoised.RawIndirectDiffuse = rawIndirectDiffuse;
        return denoised;
    }

    void ScreenSpaceGlobalIllumination::ResetHistory(
        GlobalIlluminationHistoryResetReason reason)
    {
        mImpl->HistoryTracker.Reset(reason);
        if (mImpl->Denoiser)
            mImpl->Denoiser->ResetHistory(reason);
    }

    bool ScreenSpaceGlobalIllumination::RequiresContinuousUpdate() const noexcept
    {
        return true;
    }

    GlobalIlluminationStats ScreenSpaceGlobalIllumination::GetStats() const
    {
        return mImpl->Stats;
    }

    const GlobalIlluminationOutputs&
        ScreenSpaceGlobalIllumination::GetLastRawOutputs() const noexcept
    {
        return mImpl->LastRawOutputs;
    }

    void ScreenSpaceGlobalIllumination::NotifyFrameSubmitted(
        uint64_t submission) noexcept
    {
        mImpl->Diagnostics.NotifySubmitted(submission);
        mImpl->LastSubmittedSubmission = std::max(
            mImpl->LastSubmittedSubmission,
            submission);
        if (mImpl->Denoiser)
            mImpl->Denoiser->NotifyFrameSubmitted(submission);
    }

    void ScreenSpaceGlobalIllumination::UpdateImportedResources(
        RenderGraph& renderGraph)
    {
        if (mImpl->Denoiser)
            mImpl->Denoiser->UpdateImportedResources(renderGraph);
    }

    void ScreenSpaceGlobalIllumination::AbortFrame() noexcept
    {
        mImpl->Diagnostics.Abort();
        if (mImpl->Denoiser)
            mImpl->Denoiser->AbortFrame();
    }

    void ScreenSpaceGlobalIllumination::Collect(uint64_t completedSubmission)
    {
        mImpl->Diagnostics.Collect(completedSubmission);
        if (mImpl->Denoiser)
            mImpl->Denoiser->Collect(completedSubmission);
    }

    void ScreenSpaceGlobalIllumination::Clear(uint64_t completedSubmission)
    {
        Collect(completedSubmission);
        if (mImpl->Denoiser)
            mImpl->Denoiser->Clear(completedSubmission);
        mImpl->BindingSets.clear();
        mImpl->HistoryTracker.Reset(
            GlobalIlluminationHistoryResetReason::Explicit);
        mImpl->bPrepared = false;
    }
}
