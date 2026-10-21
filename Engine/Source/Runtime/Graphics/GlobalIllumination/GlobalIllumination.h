#pragma once

#include "Runtime/Graphics/GlobalIllumination/SsgiReactiveBudget.h"

#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include "Runtime/Graphics/RHI/RenderBackend.h"
#include "Runtime/Graphics/RHI/EnvironmentSamplingProposal.h"
#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace HEngine
{
    enum class GlobalIlluminationTechniqueKind : uint8_t
    {
        Off,
        ScreenSpace,
        HardwareRayTracing,
        ExternalViewportRenderer,
    };

    enum class GlobalIlluminationDenoiserKind : uint8_t
    {
        None,
        Svgf,
        Nrd,
    };

    enum class GlobalIlluminationFallbackKind : uint8_t
    {
        Off,
        ScreenSpace,
    };

    enum class GlobalIlluminationCompositionMode : uint8_t
    {
        Replace,
        FillInvalidByCoverage,
        PreferHigherConfidence,
    };

    enum class GlobalIlluminationCompositionSource : uint8_t
    {
        None,
        Primary,
        Secondary,
    };

    [[nodiscard]] GlobalIlluminationCompositionSource
        SelectGlobalIlluminationCompositionSource(
            GlobalIlluminationCompositionMode mode,
            float primaryCoverage,
            float primaryConfidence,
            float secondaryCoverage,
            float secondaryConfidence) noexcept;

    enum class GlobalIlluminationDebugView : uint8_t
    {
        Composite,
        RawDiffuse,
        RawDirectDiffuse,
        RawIndirectDiffuse,
        Albedo,
        DemodulatedDiffuseIllumination,
        FilteredDiffuseIllumination,
        RemodulatedDiffuse,
        Normal,
        DenoisedDiffuse,
        HitDistance,
        HistoryLength,
        Confidence,
        Coverage,
        Variance,
        SourceLighting,
        TemporalDiffuseIllumination,
        ViewDepth,
        ScreenHitCoverage,
        TraceTermination,
        TraceHitLocation,
    };

    struct GlobalIlluminationSettings
    {
        GlobalIlluminationTechniqueKind Technique =
            GlobalIlluminationTechniqueKind::Off;
        GlobalIlluminationDenoiserKind Denoiser =
            GlobalIlluminationDenoiserKind::Svgf;
        GlobalIlluminationFallbackKind UnsupportedFallback =
            GlobalIlluminationFallbackKind::Off;
        GlobalIlluminationDebugView DebugView =
            GlobalIlluminationDebugView::Composite;
        float RenderScale = 1.0f;
        uint32_t SamplesPerPixel = 1;
        uint32_t MaxBounces = 4;
        uint32_t AtrousIterations = 4;
        bool bSvgfOptimizedKernels = false;
        bool bSsgiLocalReactiveBudget = false;
        bool bSvgfSharedUncertainty = false;
        bool bSsgiEnvironmentMis = true;
        uint32_t ScreenSpaceRayCount = 2;
        uint32_t ScreenSpaceStepCount = 32;
        uint32_t ScreenSpaceHiZStartMip = 2;
        uint32_t ScreenSpaceBinaryRefinementSteps = 4;
        float ScreenSpaceMaximumRayDistance = 5.0f;
        float ScreenSpaceThickness = 0.1f;
        float ScreenSpaceEdgeFade = 0.05f;
        float ScreenSpaceTemporalWeight = 0.99f;
        float ScreenSpaceDiffuseHitDistanceWeight = 1.0f;
        bool bDirectOnly = false;
        bool bBilateralUpsample = true;
        [[nodiscard]] bool operator==(const GlobalIlluminationSettings&) const = default;
    };

    struct GlobalIlluminationSelection
    {
        GlobalIlluminationTechniqueKind Requested =
            GlobalIlluminationTechniqueKind::Off;
        GlobalIlluminationTechniqueKind Selected =
            GlobalIlluminationTechniqueKind::Off;
        bool bSupported = true;
        bool bUsesHardwareRayTracing = false;
        bool bFallback = false;
        std::string Reason;
    };

    [[nodiscard]] GlobalIlluminationSelection ResolveGlobalIlluminationSelection(
        const GlobalIlluminationSettings& settings,
        const RenderCapabilities& capabilities,
        bool bScreenSpaceTechniqueAvailable);

    enum class GlobalIlluminationHistoryResetReason : uint32_t
    {
        None = 0,
        FirstFrame = 1u << 0u,
        Resize = 1u << 1u,
        RenderScale = 1u << 2u,
        CameraCut = 1u << 3u,
        Projection = 1u << 4u,
        JitterSequence = 1u << 5u,
        SceneReload = 1u << 6u,
        GeometryTopology = 1u << 7u,
        Material = 1u << 8u,
        Light = 1u << 9u,
        Technique = 1u << 10u,
        BounceCount = 1u << 11u,
        ResolutionMode = 1u << 12u,
        Denoiser = 1u << 13u,
        Sampling = 1u << 14u,
        Explicit = 1u << 15u,
        // External-renderer state that changes the accumulated integral.
        // Realtime GI leaves it stable so ordinary motion can reproject.
        ExternalRendererState = 1u << 16u,
    };

    [[nodiscard]] constexpr GlobalIlluminationHistoryResetReason operator|(
        GlobalIlluminationHistoryResetReason left,
        GlobalIlluminationHistoryResetReason right) noexcept
    {
        return static_cast<GlobalIlluminationHistoryResetReason>(
            static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
    }

    constexpr GlobalIlluminationHistoryResetReason& operator|=(
        GlobalIlluminationHistoryResetReason& left,
        GlobalIlluminationHistoryResetReason right) noexcept
    {
        left = left | right;
        return left;
    }

    [[nodiscard]] constexpr bool HasHistoryResetReason(
        GlobalIlluminationHistoryResetReason value,
        GlobalIlluminationHistoryResetReason reason) noexcept
    {
        return (static_cast<uint32_t>(value) & static_cast<uint32_t>(reason)) != 0u;
    }

    struct GlobalIlluminationHistorySignature
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        float RenderScale = 1.0f;
        uint64_t ProjectionFingerprint = 0;
        uint64_t JitterSequenceGeneration = 0;
        uint64_t SceneGeneration = 0;
        uint64_t GeometryTopologyGeneration = 0;
        // Secondary transport can change while the receiving surface remains still.
        // SSGI uses this revision for a bounded reactive history window, not a cut.
        uint64_t GeometryTransformGeneration = 0;
        uint64_t MaterialGeneration = 0;
        uint64_t LightGeneration = 0;
        uint64_t TechniqueStateFingerprint = 0;
        GlobalIlluminationTechniqueKind Technique =
            GlobalIlluminationTechniqueKind::Off;
        GlobalIlluminationDenoiserKind Denoiser =
            GlobalIlluminationDenoiserKind::None;
        uint32_t SamplesPerPixel = 1;
        uint32_t MaxBounces = 1;
        bool bHalfResolution = false;
        bool bCameraCut = false;
    };

    struct GlobalIlluminationFrameContext
    {
        RenderCamera Camera;
        GlobalIlluminationHistorySignature HistorySignature;
        SsgiReactiveRegion ReactiveRegion;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t FrameSequenceIndex = 0;
        uint32_t DepthMipCount = 1;
        uint32_t EmissiveTriangleCount = 0;
        float MaximumRayDistance = 1000.0f;
        glm::vec3 EnvironmentRadiance{ 0.0f };
        uint64_t SceneStateFingerprint = 0;
        uint64_t CompletedSubmission = 0;
    };

    struct GlobalIlluminationPrepareResult
    {
        bool bGraphRebuildRequested = false;
    };

    [[nodiscard]] GlobalIlluminationHistoryResetReason
        CompareGlobalIlluminationHistorySignatures(
            const std::optional<GlobalIlluminationHistorySignature>& previous,
            const GlobalIlluminationHistorySignature& current);

    class GlobalIlluminationHistoryTracker final
    {
    public:
        [[nodiscard]] GlobalIlluminationHistoryResetReason Update(
            const GlobalIlluminationHistorySignature& signature);
        void Reset(GlobalIlluminationHistoryResetReason reason) noexcept;
        [[nodiscard]] GlobalIlluminationHistoryResetReason GetPendingReasons() const noexcept;
        [[nodiscard]] bool HasValidHistory() const noexcept;

    private:
        std::optional<GlobalIlluminationHistorySignature> mPrevious;
        GlobalIlluminationHistoryResetReason mPendingReasons =
            GlobalIlluminationHistoryResetReason::None;
    };

    struct SceneSurfaceData
    {
        TextureHandle Depth;
        TextureHandle NormalRoughness;
        TextureHandle BaseColorMetalness;
        TextureHandle Emissive;
        TextureHandle MotionVectors;
        TextureHandle MaterialId;
        // Linear current/previous view depth for temporal reprojection. This is
        // kept separate from device depth so temporal rejection is independent
        // of near/far-plane projection precision.
        TextureHandle ViewDepth;
        // Exact stable (scene instance, mesh primitive) identity. MaterialId
        // cannot serve this purpose because unrelated surfaces may share one
        // material.
        TextureHandle SurfaceId;
        // Device depth of the current surface transformed by the previous
        // camera/object matrices. SVGF compares reprojected history against
        // this value rather than against current-frame device depth.
        TextureHandle PreviousDepth;
        // Legacy RT-subset surfaces carry material-table indices. Shared
        // raster surfaces carry validity identities and sample their own
        // material/emission textures; never index RT tables with these IDs.
        bool bMaterialIdIsRayTracingIndex = true;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return Depth && NormalRoughness && BaseColorMetalness && Emissive &&
                MotionVectors && MaterialId && ViewDepth && SurfaceId &&
                PreviousDepth;
        }
    };

    struct GlobalIlluminationInputs
    {
        SceneSurfaceData Surface;
        AccelerationStructureHandle SceneTlas;
        BufferHandle FrameBuffer;
        BufferHandle ObjectBuffer;
        BufferHandle MaterialBuffer;
        BufferHandle GeometryBuffer;
        BufferHandle LightBuffer;
        BufferHandle RayTracingVertexBuffer;
        BufferHandle RayTracingIndexBuffer;
        BufferHandle RayTracingInstanceBuffer;
        BufferHandle AnalyticLightBuffer;
        // Active scene-linear environment cubemap. Offline techniques may
        // sample this directly instead of approximating the environment with
        // a constant radiance.
        TextureHandle Environment;
        TextureHandle EnvironmentCdf;
        std::shared_ptr<const EnvironmentSamplingProposal> EnvironmentProposal;
        // Optional raster guides. PackedNormalRoughnessAo.w is authored
        // material occlusion; AmbientOcclusion is screen-space visibility.
        // They affect indirect transport only, never direct light or emission.
        TextureHandle PackedNormalRoughnessAo;
        TextureHandle AmbientOcclusion;
        // Opaque lighting before GI composition. Screen-space techniques must
        // sample this handle, never a SceneColor that already contains GI.
        TextureHandle OpaqueDirectHdr;
        TextureHandle DepthPyramid;
        glm::mat4 CurrentView{ 1.0f };
        glm::mat4 CurrentViewProjection{ 1.0f };
        glm::mat4 CurrentInverseViewProjection{ 1.0f };
        glm::mat4 PreviousViewProjection{ 1.0f };
        glm::vec3 CameraPosition{ 0.0f };
        uint32_t FrameSequenceIndex = 0;
        uint32_t Width = 0;
        uint32_t Height = 0;
    };

    struct GlobalIlluminationOutputs
    {
        // Radiance is scene-linear HDR before exposure and tone mapping.
        // CompositeRadiance is the sole final signal consumed by the raster
        // injection point. DirectDiffuse and DiffuseIndirect remain separate
        // owner AOVs and must never be added again beside CompositeRadiance.
        TextureHandle CompositeRadiance;
        TextureHandle DiffuseIndirect;
        TextureHandle DirectDiffuse;
        TextureHandle SpecularIndirect;
        // Unfiltered combined direct + indirect signal produced by the
        // technique before temporal/spatial denoising.
        TextureHandle RawDiffuse;
        TextureHandle RawDirectDiffuse;
        TextureHandle RawIndirectDiffuse;
        TextureHandle DemodulatedDiffuseIllumination;
        TextureHandle FilteredDiffuseIllumination;
        TextureHandle TemporalDiffuseIllumination;
        TextureHandle HitDistance;
        TextureHandle Confidence;
        TextureHandle CoverageMask;
        TextureHandle HistoryLength;
        TextureHandle Variance;
    };

    struct DenoiserBudgetDiagnostics
    {
        uint32_t ResponseFrames = 0;
        // 1 geometry change this frame, 2 reset pending, 4 continued response.
        uint32_t ResponseReasons = 0;
        bool bValid = false;
    };

    struct GlobalIlluminationStats
    {
        GlobalIlluminationTechniqueKind Technique =
            GlobalIlluminationTechniqueKind::Off;
        uint64_t AccumulatedSamples = 0;
        uint32_t RaysPerPixel = 0;
        SsgiReactiveRegion ReactiveRegion;
        // 0 off, 1 enabled, 2 missing proposal, 3 non-raster, 4 non-single-bounce,
        // 5 selected budget not exactly two. Alpha/hit distance follow proposal samples.
        uint32_t EnvironmentMisMode = 0;
        uint32_t FrameSequenceIndex = 0;
        uint32_t BounceCount = 0;
        uint32_t HistoryResetCount = 0;
        GlobalIlluminationHistoryResetReason LastResetReasons =
            GlobalIlluminationHistoryResetReason::None;
        double LastGpuMilliseconds = 0.0;
        std::string Status;
        // CPU selected trace budget, never a measurement of executed GPU rays.
        bool bScreenSpaceBudgetValid = false;
        uint32_t ConfiguredScreenSpaceRays = 0;
        uint32_t SelectedScreenSpaceBudgetRays = 0;
        uint32_t ScreenSpaceTraceWidth = 0;
        uint32_t ScreenSpaceTraceHeight = 0;
        DenoiserBudgetDiagnostics DenoiserBudget;
        // Unknown until optional shader instrumentation/readback is implemented.
        int64_t ExecutedScreenSpaceRays = -1;
    };

    class IGlobalIlluminationTechnique
    {
    public:
        virtual ~IGlobalIlluminationTechnique() = default;

        [[nodiscard]] virtual bool IsSupported(
            const RenderCapabilities& capabilities) const = 0;
        [[nodiscard]] virtual GlobalIlluminationPrepareResult PrepareFrame(
            const GlobalIlluminationFrameContext& frame,
            const GlobalIlluminationSettings& settings) = 0;
        [[nodiscard]] virtual GlobalIlluminationOutputs AddPasses(
            RenderGraph& renderGraph,
            const GlobalIlluminationInputs& inputs,
            const GlobalIlluminationSettings& settings) = 0;
        virtual void ResetHistory(GlobalIlluminationHistoryResetReason reason) = 0;
        virtual void UpdateImportedResources(RenderGraph& renderGraph) = 0;
        // Empty means this extension does not support candidate graph swaps.
        // Built-ins return a graph-binding exchange capsule; history is separate.
        [[nodiscard]] virtual std::function<void()> CaptureGraphState() { return {}; }
        virtual void ClearGraphResourceCaches() noexcept {}
        virtual void NotifyFrameSubmitted(uint64_t submission) noexcept = 0;
        virtual void AbortFrame() noexcept = 0;
        virtual void Collect(uint64_t completedSubmission) = 0;
        virtual void Clear(uint64_t completedSubmission) = 0;
        [[nodiscard]] virtual bool RequiresContinuousUpdate() const noexcept = 0;
        [[nodiscard]] virtual GlobalIlluminationStats GetStats() const = 0;
    };

    struct GlobalIlluminationNoisySignals
    {
        TextureHandle DiffuseIndirect;
        TextureHandle SpecularIndirect;
        TextureHandle HitDistance;
    };

    struct GlobalIlluminationDenoiserGuides
    {
        SceneSurfaceData Surface;
        TextureHandle Confidence;
        TextureHandle Coverage;
    };

    class IDenoiser
    {
    public:
        virtual ~IDenoiser() = default;

        // Optional bounded sampling feedback after PrepareFrame. This changes
        // estimator variance, not its PDF, normalization or configured quality.
        [[nodiscard]] virtual uint32_t GetMinimumRayCount() const noexcept { return 1; }
        virtual void SetReactiveRegion(SsgiReactiveRegion region) { (void)region; }
        [[nodiscard]] virtual SsgiReactiveRegion GetReactiveRegion() const noexcept { return {}; }

        [[nodiscard]] virtual DenoiserBudgetDiagnostics GetBudgetDiagnostics() const noexcept { return {}; }

        virtual void PrepareFrame(
            const GlobalIlluminationHistorySignature& signature,
            uint64_t completedSubmission)
        {
            (void)signature;
            (void)completedSubmission;
        }

        [[nodiscard]] virtual GlobalIlluminationOutputs AddPasses(
            RenderGraph& renderGraph,
            const GlobalIlluminationNoisySignals& noisySignals,
            const GlobalIlluminationDenoiserGuides& guides,
            const GlobalIlluminationSettings& settings) = 0;
        virtual void ResetHistory(GlobalIlluminationHistoryResetReason reason) = 0;
        virtual void UpdateImportedResources(RenderGraph& renderGraph)
        {
            (void)renderGraph;
        }
        virtual void NotifyFrameSubmitted(uint64_t submission) noexcept
        {
            (void)submission;
        }
        virtual void AbortFrame() noexcept {}
        [[nodiscard]] virtual bool ConsumeGraphRebuildRequest() noexcept
        {
            return false;
        }
        virtual void Collect(uint64_t completedSubmission)
        {
            (void)completedSubmission;
        }
        virtual void Clear(uint64_t completedSubmission)
        {
            Collect(completedSubmission);
        }
    };
}
