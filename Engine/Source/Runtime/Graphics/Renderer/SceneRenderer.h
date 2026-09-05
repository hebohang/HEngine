#pragma once

#include "Runtime/Graphics/RHI/TextureManager.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIllumination.h"
#include "Runtime/Graphics/Renderer/AntiAliasing.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIlluminationTechniqueRegistry.h"
#include "Runtime/Graphics/Renderer/RuntimeUiRenderGraphPass.h"
#include "Runtime/Graphics/Renderer/ViewportRendererExtension.h"
#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>
#include <functional>
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
        RenderCapabilities Capabilities;
        GlobalIlluminationSettings GlobalIllumination;
        AntiAliasingSettings AntiAliasing;
        bool bHeapDirectlyIndexed = false;

        // Allocates and records one NVRHI timer query per compiled RenderGraph
        // pass. Disabled by default so normal runtime frames have no query cost.
        bool bEnableGpuTiming = false;

        // Development-only lifecycle coverage for an initialized empty manager.
        // Production graphs add VFXTransparent only when effects are active.
        bool bRunEmptyEffekseerPassForValidation = false;
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
    };

    struct SceneRendererStats
    {
        uint32_t StaticOpaqueDrawCount = 0;
        uint32_t GpuDrivenObjectCount = 0;
        uint32_t GpuDrivenBatchCount = 0;
        uint32_t GpuVisibleObjectCount = 0;
        uint32_t GpuCulledObjectCount = 0;
        uint32_t GpuIndirectDrawCount = 0;
        uint32_t GpuComputeDispatchCount = 0;
        uint32_t CpuFallbackDrawCount = 0;
        uint32_t CustomMaterialCpuFallbackDrawCount = 0;
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
        uint32_t CachedStaticMeshCount = 0;
        uint32_t UploadedMeshCount = 0;
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
    class SceneRenderer final
    {
    public:
        explicit SceneRenderer(const SceneRendererDesc& desc);
        ~SceneRenderer();

        SceneRenderer(const SceneRenderer&) = delete;
        SceneRenderer& operator=(const SceneRenderer&) = delete;

        void RecordFrame(const RenderWorld& world, const SceneRendererFrameDesc& frame);

        // Must be called outside RecordFrame. A successful return means all
        // required GPU ShaderHandles exist and the program is now current.
        [[nodiscard]] MaterialShaderRefreshResult RefreshMaterialShaderAtFrameBoundary(
            std::string_view sourcePath);
        // RuntimeRenderer calls this for preview renderers after the shared
        // manager publishes a new generation and the device is idle.
        void ClearMaterialShaderPipelineCache();

        // Development visualization for cascade coverage and blend regions.
        // Disabled by default and does not alter the shadow resource layout.
        void SetDirectionalShadowCascadeDebug(bool bEnabled) noexcept;

        // Development-only PBR component visualization. Disabled by default;
        // all modes keep the normal HDR/tone-map output path intact.
        void SetPbrDebugMode(ScenePbrDebugMode mode) noexcept;

        // Configure outside RecordFrame. RuntimeRenderer waits for the device
        // before changing techniques so persistent histories can retire safely.
        void SetGlobalIlluminationSettings(
            const GlobalIlluminationSettings& settings);
        void SetAntiAliasingSettings(const AntiAliasingSettings& settings);
        void SetRenderSettings(
            const AntiAliasingSettings& antiAliasing,
            const GlobalIlluminationSettings& globalIllumination);
        [[nodiscard]] const AntiAliasingSettings&
            GetAntiAliasingSettings() const noexcept;
        [[nodiscard]] const AntiAliasingSettings&
            GetRequestedAntiAliasingSettings() const noexcept;
        [[nodiscard]] const AntiAliasingSettings&
            GetEffectiveAntiAliasingSettings() const noexcept;
        [[nodiscard]] const AntiAliasingCapabilities&
            GetAntiAliasingCapabilities() const noexcept;
        [[nodiscard]] AntiAliasingSelection
            GetAntiAliasingSelection() const;
        void RestartAntiAliasingConvergence() noexcept;
        void ResetGlobalIlluminationHistory(
            GlobalIlluminationHistoryResetReason reason =
                GlobalIlluminationHistoryResetReason::Explicit);
        [[nodiscard]] const GlobalIlluminationSettings&
            GetGlobalIlluminationSettings() const noexcept;
        [[nodiscard]] const GlobalIlluminationSettings&
            GetRequestedGlobalIlluminationSettings() const noexcept;
        [[nodiscard]] const GlobalIlluminationSettings&
            GetEffectiveGlobalIlluminationSettings() const noexcept;
        [[nodiscard]] GlobalIlluminationSelection
            GetGlobalIlluminationSelection() const;
        [[nodiscard]] std::vector<GlobalIlluminationTechniqueDescriptor>
            EnumerateGlobalIlluminationTechniques() const;
        [[nodiscard]] bool RequiresContinuousUpdate() const noexcept;
        [[nodiscard]] nvrhi::TextureHandle GetOfflineViewportBeautyTexture() const;
        void ActivateOfflineViewportRenderer(std::string providerId);
        void DeactivateOfflineViewportRenderer();
        [[nodiscard]] std::string GetActiveOfflineViewportRendererId() const;
        [[nodiscard]] IOfflineViewportRendererSession*
            GetOfflineViewportRendererSession() const noexcept;
        [[nodiscard]] std::vector<OfflineViewportRendererProviderDescriptor>
            EnumerateOfflineViewportRenderers() const;

        // Synchronously decodes and precomputes the IBL cache on NVRHI. Call
        // outside an active frame (normally during Game startup). Empty paths
        // represent an intentionally unlit environment and are immediately Ready.
        [[nodiscard]] SceneEnvironmentPreparation PrepareEnvironment(
            std::string_view path);
        [[nodiscard]] SceneEnvironmentPreparation GetEnvironmentPreparation(
            std::string_view path) const;
        [[nodiscard]] bool IsEnvironmentReady(std::string_view path) const;

        // Shared typed texture ownership for runtime/editor UI. Load outside an
        // active RecordFrame; descriptors are collected with frame submission.
        [[nodiscard]] ManagedTextureHandle LoadTexture(const TextureLoadDesc& desc);
        [[nodiscard]] const ManagedTexture& GetTexture(ManagedTextureHandle handle) const;
        void ReleaseTexture(
            ManagedTextureHandle handle,
            uint64_t retireAfterSubmission);
        size_t InvalidateFailedTextureLoads(std::string_view assetPath = {});

        // Configure outside an active frame. SwapChainAndOffscreen copies the
        // tone-mapped LDR scene after the runtime RmlUi composite and before
        // the window ImGui overlay; OffscreenOnly renders a dedicated preview
        // without UI/capture/present. A dedicated preview SceneRenderer can
        // record into the same open command list as the main renderer and share
        // the eventual RenderDevice submission.
        void ConfigureOffscreenOutput(
            uint32_t width,
            uint32_t height,
            std::string debugName = "SceneRenderer Offscreen Output");
        void DisableOffscreenOutput();
        [[nodiscard]] SceneRendererOffscreenOutput GetOffscreenOutput() const;

        // Submission is known only after RenderDevice executes the command list.
        // This call closes the frame-slot ownership interval used by transient
        // RenderGraph resources.
        void NotifyFrameSubmitted(uint32_t frameSlot, uint64_t submission);
        void AbortFrame(uint32_t frameSlot) noexcept;

        // Call after RenderDevice::WaitForIdle and before resizing the swapchain;
        // this releases imported backbuffer/framebuffer references required by
        // DXGI ResizeBuffers.
        void Resize(uint32_t width, uint32_t height);
        void ClearCaches();

        // Maps the per-frame indirect-argument readbacks. The caller must first
        // wait for the graphics queue to become idle.
        void CollectGpuCullingStats();

        // Resolves all submitted timer queries. The caller must first wait for
        // the graphics queue to become idle.
        void CollectGpuTimingStats();

        [[nodiscard]] const SceneRendererStats& GetStats() const noexcept;
        [[nodiscard]] RenderGraphDiagnosticSnapshot
            GetRenderGraphDiagnosticSnapshot() const;
        [[nodiscard]] std::string BuildRenderGraphDebugDump() const;
        [[nodiscard]] std::string BuildGpuTimingDebugDump() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
