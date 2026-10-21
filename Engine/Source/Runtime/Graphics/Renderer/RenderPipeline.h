#pragma once

#include "Runtime/Graphics/RHI/TextureManager.h"
#include "Runtime/Graphics/RHI/FrameCaptureSource.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIllumination.h"
#include "Runtime/Graphics/Renderer/AntiAliasing.h"
#include "Runtime/Graphics/Renderer/Upscaling.h"
#include "Runtime/Graphics/Renderer/RenderingPath.h"
#include "Runtime/Graphics/Renderer/ScenePostProcessPass.h"
#include "Runtime/Graphics/Renderer/SceneAmbientOcclusionPass.h"
#include "Runtime/Graphics/Renderer/SceneCloudPass.h"
#include "Runtime/Graphics/Renderer/SceneReflectionPass.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIlluminationTechniqueRegistry.h"
#include "Runtime/Graphics/Renderer/RuntimeUiRenderGraphPass.h"
#include "Runtime/Graphics/Renderer/ViewportRendererExtension.h"
#include "Runtime/Graphics/Scene/RenderWorld.h"
#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Graphics/Renderer/RenderFeature.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
    class MaterialShaderManager;
    class ShaderManager;
    struct MaterialShaderManifest;
    struct MaterialShaderProgramIdentity;
    struct MaterialShaderRefreshResult;

    struct SceneRendererOverlayContext
    {
        nvrhi::ICommandList* CommandList = nullptr;
        nvrhi::IFramebuffer* Framebuffer = nullptr;
        nvrhi::ITexture* BackBuffer = nullptr;
        nvrhi::ITexture* StencilTarget = nullptr;
        nvrhi::ITexture* BackdropTexture = nullptr;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t FrameSlot = 0;
        uint64_t CompletedSubmission = 0;
    };

    struct SceneRendererCaptureContext
    {
        nvrhi::ICommandList* CommandList = nullptr;
        nvrhi::ITexture* BackBuffer = nullptr;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t FrameSlot = 0;
        uint64_t CompletedSubmission = 0;
    };

    struct SceneRendererLinearHdrCaptureContext
    {
        nvrhi::ICommandList* CommandList = nullptr;
        nvrhi::ITexture* LinearHdr = nullptr;
        // Native debug AOV before full-resolution composition/FP16 conversion.
        // Optional; raw exports preserve its format, dimensions and alpha.
        nvrhi::ITexture* DiagnosticInput = nullptr;
        nvrhi::ITexture* SelectedNativeInput = nullptr;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t FrameSlot = 0;
        uint64_t CompletedSubmission = 0;
    };

    using SceneRendererOverlayRecorder =
        std::function<void(const SceneRendererOverlayContext&)>;
    using SceneRendererCaptureRecorder =
        std::function<void(const SceneRendererCaptureContext&)>;
    using SceneRendererLinearHdrCaptureRecorder =
        std::function<void(const SceneRendererLinearHdrCaptureContext&)>;

    enum class SceneRendererOutputMode : uint8_t
    {
        SwapChain,
        SwapChainAndOffscreen,
        OffscreenOnly,
    };

    enum class ScenePbrDebugMode : uint8_t
    {
        Disabled = 0,
        BaseColor = 1,
        WorldNormal = 2,
        Metallic = 3,
        Roughness = 4,
        AmbientOcclusion = 5,
        DirectLighting = 6,
        DiffuseIbl = 7,
        SpecularIbl = 8,
        Shadow = 9,
        FinalHdr = 10,
        HdrValidity = 11,
        ScreenSpaceAo = 12,
        ViewDepth = 13,
    };

    // SceneRenderer owns these NVRHI resources. Generation changes whenever a
    // resize replaces them, so UI texture registries can retire stale views.
    struct SceneRendererOffscreenOutput
    {
        nvrhi::TextureHandle Color;
        nvrhi::TextureHandle Depth;
        uint32_t Width = 0;
        uint32_t Height = 0;
        nvrhi::Format ColorFormat = nvrhi::Format::RGBA8_UNORM;
        nvrhi::Format DepthFormat = nvrhi::Format::D32;
        uint64_t Generation = 0;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return Color && Depth && Width > 0 && Height > 0 && Generation > 0;
        }
    };

    struct SceneRendererDesc
    {
        std::string PipelineType = "realtime";
        Json PipelineFeatures = DefaultRenderFeatureConfigurations();
        uint64_t TransientCacheBudgetBytes = std::numeric_limits<uint64_t>::max();
        uint64_t TextureStreamingBudgetBytes = 512ull * 1024 * 1024;
        nvrhi::IDevice* Device = nullptr;
        uint32_t FramesInFlight = 3;
        std::filesystem::path ShaderCacheDirectory;
        std::filesystem::path AssetRootDirectory;
        std::filesystem::path EngineAssetRootDirectory;
        // RuntimeRenderer supplies one shared material-program manager for the
        // main scene and all preview renderers. This keeps program generations
        // globally comparable in immutable Material snapshots.
        MaterialShaderManager* SharedMaterialShaderManager = nullptr;
        ShaderManager* SharedMaterialShaderHandleManager = nullptr;
        RenderBackend Backend = RenderBackend::DX12;
        RenderingPath Path = RenderingPath::Deferred;
        RenderCapabilities Capabilities;
        GlobalIlluminationSettings GlobalIllumination;
        AntiAliasingSettings AntiAliasing;
        UpscalingSettings Upscaling;
        ScenePostProcessSettings PostProcess;
        SceneAmbientOcclusionSettings AmbientOcclusion;
        CloudSettings Clouds;
        SceneReflectionSettings Reflections;
        bool bGpuOcclusionEnabled = false;
        bool bGpuBatchingEnabled = false;
        bool bGpuTightFrustumEnabled = false;
        bool bGpuShadowBatchingEnabled = false;
        bool bTransientTextureReuse = false;
        bool bLodEnabled = false;
        bool bShadowsEnabled = true;
        bool bContinuousPcfEnabled = true;
        float LodMaxPixelError = 1.0f;
        bool bHeapDirectlyIndexed = false;

        // Allocates and records one NVRHI timer query per compiled RenderGraph
        // pass. Disabled by default so normal runtime frames have no query cost.
        bool bEnableGpuTiming = false;
        bool bEnableCpuGraphTiming = false;

        // Development-only lifecycle coverage for an initialized empty manager.
        // Production graphs add VFXTransparent only when effects are active.
        bool bRunEmptyEffekseerPassForValidation = false;
    };

    struct SceneRendererConfigurationPreparationResult
    {
        // False for a view that has never rendered: its first graph is pending.
        bool bGraphPrepared = false;
        uint64_t CandidatePoolNominalBytes = 0;
        double PreparationMilliseconds = 0.0;
    };

    enum class SceneEnvironmentState : uint8_t
    {
        Unrequested,
        Preparing,
        Ready,
        Failed
    };

    struct SceneEnvironmentPreparation
    {
        SceneEnvironmentState State = SceneEnvironmentState::Unrequested;
        std::string Path;
        std::string Error;
    };

    struct SceneRendererFrameDesc
    {
        nvrhi::ICommandList* CommandList = nullptr;
        nvrhi::TextureHandle BackBuffer;
        nvrhi::Format BackBufferFormat = nvrhi::Format::RGBA8_UNORM;
        nvrhi::ResourceStates BackBufferInitialState =
            nvrhi::ResourceStates::Present;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t FrameSlot = 0;
        uint64_t CompletedSubmission = 0;
        SceneRendererOutputMode OutputMode = SceneRendererOutputMode::SwapChain;
		// Editor viewports can reuse their persistent offscreen scene image while
		// the immutable RenderWorld snapshot is unchanged. Only UI/capture/present
		// are recorded for such frames.
        bool bReuseOffscreenScene = false;
        RuntimeUiRenderMode SceneOverlayMode = RuntimeUiRenderMode::Direct;
        RuntimeUiTransitionParameters SceneOverlayTransition;

        // These recorders are copied for this RecordFrame call and invoked
        // synchronously from graph-owned passes. They are released before
        // RecordFrame returns and must not retain the supplied NVRHI pointers.
        SceneRendererOverlayRecorder SceneOverlayRecorder;
        SceneRendererOverlayRecorder WindowOverlayRecorder;
        SceneRendererCaptureRecorder CaptureRecorder;
        SceneRendererLinearHdrCaptureRecorder LinearHdrCaptureRecorder;
        FrameCaptureSource CaptureSource = FrameCaptureSource::Automatic;
        Ref<const RenderWorld> WorldSnapshot;
    };

    struct SceneGpuPassTiming
    {
        std::string Name;
        double Milliseconds = 0.0;
    };

    struct SceneCpuGraphPhaseTiming
    {
        const char* Phase = ""; // Static phase label; never a transient string view.
        uint8_t PhaseIndex = 0;
        uint32_t ExecutionIndex = UINT32_MAX;
        std::string Name;
        double Milliseconds = 0.0;
        bool bCompleted = false;
    };

    namespace SceneGraphRebuildReason
    {
        inline constexpr uint32_t ExistingDirty = 1u << 0;
        inline constexpr uint32_t LocalShadow = 1u << 1;
        inline constexpr uint32_t PostProcess = 1u << 2;
        inline constexpr uint32_t Feature = 1u << 3;
        inline constexpr uint32_t AmbientOcclusion = 1u << 4;
        inline constexpr uint32_t Reflection = 1u << 5;
        inline constexpr uint32_t Vfx = 1u << 6;
        inline constexpr uint32_t Output = 1u << 7;
        inline constexpr uint32_t TextureTable = 1u << 8;
        inline constexpr uint32_t PostTaaDepth = 1u << 9;
        inline constexpr uint32_t MeshAdded = 1u << 10;
        inline constexpr uint32_t MeshUploaded = 1u << 11;
    }

    struct SceneRendererStats
    {
        bool bCpuCounterFrameValid = false;
        uint64_t CpuCounterWorldFrame = 0;
        uint32_t InternalWidth = 0;
        uint32_t InternalHeight = 0;
        uint32_t OutputWidth = 0;
        uint32_t OutputHeight = 0;
        bool bUpscalingActive = false;
        bool bUpscalingFailed = false;
        UpscalingStatus UpscalingState = UpscalingStatus::Off;
        std::string UpscalingFailureReason;
        uint64_t InputRenderObjects = 0;
        uint64_t PreparedRasterSections = 0;
        uint64_t SectionsWithAuthoredLods = 0;
        // Instance-section distribution, not unique asset coverage.
        std::array<uint64_t, 9> SelectedLodSections{};
        std::array<uint64_t, 4> DirectionalCpuCasterDraws{};
        uint32_t LocalShadowActiveViews = 0;
        uint32_t LocalShadowDirtyViews = 0;
        uint32_t LocalShadowCachedViews = 0;
        uint32_t LocalShadowVisibleCasters = 0;
        double CloudCpuRecordMilliseconds = 0;
        uint32_t StaticOpaqueDrawCount = 0;
        uint32_t GpuDrivenObjectCount = 0;
        uint32_t GpuDrivenBatchCount = 0;
        uint32_t MainAabbTestedCandidates = 0;
        uint32_t MainAabbRejectedCandidates = 0;
        uint64_t MainAabbRejectedCandidateTriangles = 0;
        uint32_t GpuVisibleObjectCount = 0;
        uint32_t GpuCulledObjectCount = 0;
        uint32_t GpuIndirectDrawCount = 0;
        uint32_t GpuComputeDispatchCount = 0;
        uint32_t CpuFallbackDrawCount = 0;
        uint32_t CustomMaterialCpuFallbackDrawCount = 0;
        uint32_t CustomMaterialGpuDrawCount = 0;
        uint32_t CustomMaterialGpuObjectCount = 0;
        uint32_t MaterialProgramCount = 0;
        uint32_t MaterialParameterRowCount = 0;
        uint64_t MaterialProgramGenerationMax = 0;
        uint32_t CpuTransparentDrawCount = 0;
        uint32_t AlphaMaskDrawCount = 0;
        uint32_t DepthPrepassDrawCount = 0;
        uint32_t DepthPrepassIndirectDrawCount = 0;
        uint32_t DepthPrepassSkinnedDrawCount = 0;
        uint32_t DepthPrepassAlphaMaskDrawCount = 0;
        uint32_t SkinnedDrawCount = 0;
        uint32_t SkinnedBoneMatrixCount = 0;
        uint32_t SkinnedNonIdentityBoneMatrixCount = 0;
        uint32_t EditorOutlineDrawCount = 0;
        uint32_t GpuInstancedBatchCount = 0;
        uint32_t GpuMaxVisibleInstancesPerBatch = 0;
        // Actual depth submissions, including one submission per active cascade.
        uint32_t DirectionalShadowDrawCount = 0;
        uint32_t DirectionalShadowCascadeCount = 0;
        double GpuRenderGraphAverageMilliseconds = 0.0;
        double GpuRenderGraphLastMilliseconds = 0.0;
        uint64_t GpuRenderGraphSampleCount = 0;
        // Submission owning the last completed graph query sample, not the current CPU frame.
        uint64_t GpuRenderGraphLastSubmissionId = 0;
        // One timestamp interval surrounding the entire graph, including gaps
        // and resource barriers. Separate from the sum of per-pass intervals.
        double GpuFrameSpanMilliseconds = 0.0;
        uint64_t SceneTableUploadedBytes = 0;
        // CPU PrepareFrameData scope; excludes graph recording/submission.
        double CpuFramePreparationMilliseconds = 0.0;
        double CpuGraphRecordMilliseconds = 0.0;
        double CpuGraphBuildMilliseconds = 0.0;
        uint32_t GraphBuildReasonMask = 0;
        double CpuMaterialPackingMilliseconds = 0.0;
        uint32_t MaterialPackingCallCount = 0;
        uint32_t MaterialPackingCacheHitCount = 0;
        bool bMaterialPackingCacheEnabled = true;
        uint64_t GeometryArenaReservedBytes = 0;
        uint64_t GeometryArenaUsedBytes = 0;
        uint64_t IndirectCompactionResidentBytes = 0;
        uint64_t IndirectCompactionUploadedBytes = 0;
        uint32_t GpuMultiDrawGroupCount = 0;
        uint32_t GpuShadowIndirectDrawCount = 0;
        uint32_t GpuShadowMultiDrawGroupCount = 0;
        uint32_t GpuShadowRecordedViews = 0;
        uint32_t GpuShadowCompactedViews = 0;
        uint64_t GpuShadowResidentBytes = 0;
        uint64_t GpuShadowUploadedBytes = 0;
        // Latest completed GPU sample only; never a mix of retired graph passes.
        std::vector<SceneGpuPassTiming> GpuPassTimings;
        // Current CPU recording; submission is filled only by NotifyFrameSubmitted.
        std::vector<SceneCpuGraphPhaseTiming> CpuGraphTimings;
        uint32_t CpuGraphTimingCount = 0;
        uint64_t CpuGraphSubmissionId = 0;
        bool bCpuGraphTimingValid = false;
        uint64_t GpuVisibleTriangleCount = 0;
        // Identity/scope of the completed indirect-argument readback above.
        // Zero submission means no completed sample, not zero visibility.
        uint64_t GpuCullingReadbackSubmission = 0;
        uint32_t GpuCullingReadbackInputObjects = 0;
        uint32_t GpuCullingReadbackBatches = 0;
        uint64_t BaseRasterTriangleCount = 0;
        uint64_t SelectedRasterTriangleCount = 0;
        uint32_t ReducedLodSectionCount = 0;
        uint32_t CachedStaticMeshCount = 0;
        uint32_t UploadedMeshCount = 0;
        // Includes decoded, queued and GPU-submitted textures awaiting publication.
        uint32_t PendingTextureUploads = 0;
        uint32_t BindlessMaterialCount = 0;
        uint32_t GpuMaterialCount = 0;
        uint32_t BindlessTextureCount = 0;
        uint32_t MaterialDescriptorPageCount = 0;
        uint32_t MaterialDescriptorPeakPageTextures = 0;
        // Number of page binding sets created in the latest frame. Stable
        // pages are cached, so an unchanged scene reports zero.
        uint32_t MaterialBindingSetCreationCount = 0;
        uint32_t Primitive2DCount = 0;
        uint32_t Primitive2DBatchCount = 0;
        uint32_t Primitive2DVertexCount = 0;
        bool bVfxPassAdded = false;
        bool bVfxDepthSnapshotAdded = false;
        bool bVfxBackgroundSnapshotAdded = false;
        bool bOverlayPassRecorded = false;
        bool bCapturePassRecorded = false;
        bool bLinearHdrCaptureRecorded = false;
        bool bOffscreenOutputRecorded = false;
		bool bSceneReused = false;
        bool bEnvironmentReady = true;
        bool bGpuTimingEnabled = false;
        std::string EnvironmentPath;
        nvrhi::Format SceneColorFormat = nvrhi::Format::RGBA16_FLOAT;
        nvrhi::Format SceneDepthFormat = nvrhi::Format::D32;
        std::vector<std::string> CompiledPasses;
        GlobalIlluminationStats GiStats;
        uint32_t RayTracingBlasBuildCount = 0;
        uint32_t RayTracingBlasReuseCount = 0;
        uint32_t RayTracingTlasBuildCount = 0;
        uint32_t RayTracingTlasUpdateCount = 0;
        uint32_t RayTracingInstanceCount = 0;
        uint32_t RayTracingExcludedInstanceCount = 0;
        uint32_t RayTracingEmissiveTriangleCount = 0;
        uint64_t RayTracingGeometryBytes = 0;
        uint64_t RayTracingSceneTableBytes = 0;
        uint64_t RayTracingActiveBufferAllocationRequirementBytes = 0;
        uint64_t RayTracingActiveAsAllocationRequirementBytes = 0;
        // Completed RenderGraph timestamp samples. BLAS is the sum of the
        // peak non-noop sample for each current BLAS pass; TLAS is the latest
        // combined build/update pass sample.
        double GpuRayTracingBlasBuildEventMilliseconds = 0.0;
        double GpuRayTracingTlasBuildEventMilliseconds = 0.0;
        double GpuRayTracingTlasUpdateMilliseconds = 0.0;
        double GpuGiTraceMilliseconds = 0.0;
        double GpuSvgfMilliseconds = 0.0;
        double GpuGiCompositeMilliseconds = 0.0;
        bool bGpuGiTimingValid = false;
        AntiAliasingMode RequestedAntiAliasing = AntiAliasingMode::None;
        AntiAliasingMode EffectiveAntiAliasing = AntiAliasingMode::None;
        uint32_t RasterSampleCount = 1;
        bool bAntiAliasingFallback = false;
        uint64_t RenderGraphPersistentBytes = 0;
        uint64_t RenderGraphTransientReservedBytes = 0;
        uint64_t RenderGraphCachedTransientBytes = 0;
        uint64_t RenderGraphRetiredResourceBytes = 0;
        uint64_t RenderGraphTrimmedTransientBytes = 0;
        uint64_t RenderGraphPeakFrameTransientBytes = 0;
    };

    // Pure compatibility predicates shared by frame preparation and headless
    // tests. Generation 0 is the cold-load sentinel; every content/layout
    // identity field must still match before a program may be used.
    [[nodiscard]] bool IsMaterialSnapshotProgramCompatible(
        const RenderMeshSection::MaterialInstanceSnapshot& snapshot,
        const MaterialShaderProgramIdentity& programIdentity,
        uint64_t programGeneration) noexcept;
    [[nodiscard]] bool IsMaterialSnapshotRenderStateCompatible(
        const RenderMeshSection::MaterialInstanceSnapshot& snapshot,
        const MaterialShaderManifest& manifest) noexcept;

    struct TransparentDrawSortKey
    {
        float Depth = 0.0f;
        int EntityId = -1;
        uint32_t SectionIndex = 0;
        uint64_t SubmissionOrder = 0;
    };

    // Transparent sections of one multi-part mesh can have different local
    // centers. Sorting uses that section center transformed to view space,
    // rather than the aggregate RenderObject bounds.
    [[nodiscard]] float ComputeTransparentSubMeshSortDepth(
        const glm::mat4& view,
        const glm::mat4& world,
        bool bHasLocalBounds,
        const glm::vec3& localBoundsMinimum,
        const glm::vec3& localBoundsMaximum) noexcept;
    [[nodiscard]] bool IsTransparentDrawSortKeyBefore(
        const TransparentDrawSortKey& left,
        const TransparentDrawSortKey& right) noexcept;

    // Records the modern HDR scene graph (opaque GPU/CPU paths, shadows, IBL,
    // extracted 2D primitives, VFX, tone mapping, UI/capture and present) into
    // an already-open NVRHI command list. RenderWorld is the only scene input;
    // this class never reaches into ECS or the legacy immediate renderer.
    // Host-facing pipeline contract. Device submission and presentation remain
    // in RuntimeRenderer; implementations own per-view graph and histories.
    class RenderPipeline
    {
    public:
        virtual ~RenderPipeline() = default;
        virtual void RecordFrame(const RenderWorld& world, const SceneRendererFrameDesc& frame) = 0;
        // Capture-only, queried immediately after recording and BEFORE NotifyFrameSubmitted.
        // Default implementations cannot claim raster matrix provenance.
        [[nodiscard]] virtual Json GetRecordedRasterCaptureMetadata() const
        { return Json{ {"valid", false}, {"reason", "pipeline-raster-metadata-unavailable"} }; }
        [[nodiscard]] virtual bool IsCaptureSourceAvailable(FrameCaptureSource source) const noexcept
        { return source == FrameCaptureSource::Automatic; }

        [[nodiscard]] virtual MaterialShaderRefreshResult RefreshMaterialShaderAtFrameBoundary(
            std::string_view sourcePath) = 0;
        virtual void ClearMaterialShaderPipelineCache() = 0;
        virtual void SetDirectionalShadowCascadeDebug(bool bEnabled) noexcept = 0;
        virtual void SetPbrDebugMode(ScenePbrDebugMode mode) noexcept = 0;
        virtual void SetGlobalIlluminationSettings(
            const GlobalIlluminationSettings& settings) = 0;
        virtual void SetAntiAliasingSettings(const AntiAliasingSettings& settings) = 0;
        [[nodiscard]] virtual SceneRendererConfigurationPreparationResult PrepareRenderingConfiguration(
            const SceneRendererDesc& target, const RenderWorld& world,
            const SceneRendererFrameDesc* preparationFrame = nullptr) = 0;
        virtual void CommitPreparedRenderingConfiguration() noexcept = 0;
        virtual void AbortPreparedRenderingConfiguration() noexcept = 0;
        virtual void PrepareRenderingPath(RenderingPath path) = 0;
        virtual void SetPostProcessSettings(const ScenePostProcessSettings& settings) = 0;
        virtual void SetUpscalingSharpness(float sharpness) {}
        virtual void SetCloudSettings(const CloudSettings& settings) = 0;
        virtual void SetAmbientOcclusionSettings(const SceneAmbientOcclusionSettings& settings) = 0;
        virtual void SetReflectionSettings(const SceneReflectionSettings& settings) = 0;
        virtual void SetGpuOcclusionEnabled(bool bEnabled) = 0;
        virtual void SetGpuBatchingEnabled(bool bEnabled) = 0;
        virtual void SetGpuShadowBatchingEnabled(bool bEnabled) = 0;
        virtual void SetLodSettings(bool bEnabled, float maxPixelError) = 0;
        virtual void PrepareReflectionProbeResources(const RenderWorld& world) = 0;
        virtual void CommitRenderingPath(RenderingPath path) = 0;
        [[nodiscard]] virtual RenderingPath GetRenderingPath() const noexcept = 0;
        virtual void SetRenderSettings(
            const AntiAliasingSettings& antiAliasing,
            const GlobalIlluminationSettings& globalIllumination) = 0;
        [[nodiscard]] virtual const AntiAliasingSettings&
            GetAntiAliasingSettings() const noexcept = 0;
        [[nodiscard]] virtual const AntiAliasingSettings&
            GetRequestedAntiAliasingSettings() const noexcept = 0;
        [[nodiscard]] virtual const AntiAliasingSettings&
            GetEffectiveAntiAliasingSettings() const noexcept = 0;
        [[nodiscard]] virtual const AntiAliasingCapabilities&
            GetAntiAliasingCapabilities() const noexcept = 0;
        [[nodiscard]] virtual AntiAliasingSelection
            GetAntiAliasingSelection() const = 0;
        virtual void RestartAntiAliasingConvergence() noexcept = 0;
        virtual void ResetTemporalHistory() = 0;
        virtual void ResetGlobalIlluminationHistory(
            GlobalIlluminationHistoryResetReason reason =
                GlobalIlluminationHistoryResetReason::Explicit) = 0;
        [[nodiscard]] virtual const GlobalIlluminationSettings&
            GetGlobalIlluminationSettings() const noexcept = 0;
        [[nodiscard]] virtual const GlobalIlluminationSettings&
            GetRequestedGlobalIlluminationSettings() const noexcept = 0;
        [[nodiscard]] virtual const GlobalIlluminationSettings&
            GetEffectiveGlobalIlluminationSettings() const noexcept = 0;
        [[nodiscard]] virtual GlobalIlluminationSelection
            GetGlobalIlluminationSelection() const = 0;
        [[nodiscard]] virtual std::vector<GlobalIlluminationTechniqueDescriptor>
            EnumerateGlobalIlluminationTechniques() const = 0;
        [[nodiscard]] virtual bool RequiresContinuousUpdate() const noexcept = 0;
        [[nodiscard]] virtual nvrhi::TextureHandle GetOfflineViewportBeautyTexture() const = 0;
        virtual void ActivateOfflineViewportRenderer(std::string providerId) = 0;
        virtual void DeactivateOfflineViewportRenderer() = 0;
        [[nodiscard]] virtual std::string GetActiveOfflineViewportRendererId() const = 0;
        [[nodiscard]] virtual IOfflineViewportRendererSession*
            GetOfflineViewportRendererSession() const noexcept = 0;
        [[nodiscard]] virtual std::vector<OfflineViewportRendererProviderDescriptor>
            EnumerateOfflineViewportRenderers() const = 0;
        [[nodiscard]] virtual SceneEnvironmentPreparation PrepareEnvironment(
            std::string_view path) = 0;
        [[nodiscard]] virtual SceneEnvironmentPreparation GetEnvironmentPreparation(
            std::string_view path) const = 0;
        [[nodiscard]] virtual bool IsEnvironmentReady(std::string_view path) const = 0;
        [[nodiscard]] virtual ManagedTextureHandle LoadTexture(const TextureLoadDesc& desc) = 0;
        [[nodiscard]] virtual const ManagedTexture& GetTexture(ManagedTextureHandle handle) const = 0;
        virtual void ReleaseTexture(
            ManagedTextureHandle handle,
            uint64_t retireAfterSubmission) = 0;
        virtual size_t InvalidateFailedTextureLoads(std::string_view assetPath = {}) = 0;
        virtual size_t RequestTextureReload(std::string_view assetPath) = 0;
#if defined(HE_DEV_AGENT_RUNTIME)
        virtual bool InjectUpscalingFailureForValidation(bool) { return false; }
#endif
        virtual void ConfigureOffscreenOutput(
            uint32_t width,
            uint32_t height,
            std::string debugName = "SceneRenderer Offscreen Output",
            nvrhi::Format colorFormat = nvrhi::Format::RGBA8_UNORM) = 0;
        virtual void DisableOffscreenOutput() = 0;
        [[nodiscard]] virtual SceneRendererOffscreenOutput GetOffscreenOutput() const = 0;
        virtual void NotifyFrameSubmitted(uint32_t frameSlot, uint64_t submission) = 0;
        virtual void AbortFrame(uint32_t frameSlot) noexcept = 0;
        virtual void Resize(uint32_t width, uint32_t height) = 0;
        virtual void ClearCaches() = 0;
        virtual void SetTransientCacheBudget(uint64_t bytes) noexcept = 0;
        virtual void SetTextureStreamingBudget(uint64_t bytes) noexcept {}
        virtual void CollectGpuCullingStats() = 0;
        virtual void CollectGpuTimingStats() = 0;
        [[nodiscard]] virtual const SceneRendererStats& GetStats() const noexcept = 0;
        [[nodiscard]] virtual RenderGraphDiagnosticSnapshot
            GetRenderGraphDiagnosticSnapshot() const = 0;
        [[nodiscard]] virtual std::string BuildRenderGraphDebugDump() const = 0;
        [[nodiscard]] virtual std::string BuildGpuTimingDebugDump() const = 0;
    };
}
