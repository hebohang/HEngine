#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/RHI/RenderBackend.h"
#include "Runtime/Graphics/RHI/RenderDevice.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIllumination.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIlluminationTechniqueRegistry.h"
#include "Runtime/Graphics/RHI/TextureReadback.h"
#include "Runtime/Graphics/RHI/FrameCaptureSource.h"
#include "Runtime/Graphics/RHI/TextureManager.h"
#include "Runtime/Graphics/Renderer/AntiAliasing.h"
#include "Runtime/Graphics/Renderer/Upscaling.h"
#include "Runtime/Graphics/Renderer/RenderingPath.h"
#include "Runtime/Graphics/Renderer/ScenePostProcessPass.h"
#include "Runtime/Graphics/Renderer/SceneAmbientOcclusionPass.h"
#include "Runtime/Graphics/Renderer/SceneCloudPass.h"
#include "Runtime/Graphics/Renderer/SceneReflectionPass.h"
#include "Runtime/Graphics/Renderer/RuntimeUiRenderGraphPass.h"
#include "Runtime/Graphics/Renderer/ViewportRendererExtension.h"
#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Graphics/Renderer/RenderFeature.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
    class RenderSurface;
    struct MaterialShaderRefreshResult;
    struct RenderWorld;
    struct SceneRendererStats;
    struct ResolvedRenderSettings;
    struct RenderSettingsChangePlan;
    struct RenderGraphDiagnosticSnapshot;

    struct RuntimeRendererDesc
    {
        std::string PipelineType = "realtime";
        Json PipelineFeatures = DefaultRenderFeatureConfigurations();
        uint64_t TransientCacheBudgetBytes = std::numeric_limits<uint64_t>::max();
        uint64_t TextureStreamingBudgetBytes = 512ull * 1024 * 1024;
        RenderSurface* Surface = nullptr;
		RenderBackend Backend = RenderBackend::DX12;
        RenderingPath Path = RenderingPath::Deferred;
        RenderAdapterSelectionOptions AdapterSelection;
        uint32_t FramesInFlight = 3;
        std::filesystem::path ShaderCacheDirectory;
        std::filesystem::path AssetRootDirectory;
        std::filesystem::path EngineAssetRootDirectory;
        bool bEnableValidation = false;
        bool bEnableGpuTiming = false;
        bool bEnableCpuGraphTiming = false;
        bool bEnableGpuFrameMarkers = true;
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
        std::uint32_t PbrDebugMode = 0;
        bool bShadowCascadeDebug = false;
    };

    struct RuntimeConfigurationPreparationStats
    {
        uint64_t AttemptId = 0;
        uint32_t PreparedViews = 0;
        bool bCommitted = false;
        double PreparationMilliseconds = 0.0;
        double TransactionMilliseconds = 0.0;
        uint64_t ObservedActivePoolNominalBytes = 0;
        uint64_t PreparedCandidatePoolNominalBytes = 0;
    };

    enum class RuntimeEnvironmentState : uint8_t
    {
        Unrequested,
        Preparing,
        Ready,
        Failed,
    };

    struct RuntimeEnvironmentPreparation
    {
        RuntimeEnvironmentState State = RuntimeEnvironmentState::Unrequested;
        std::string Path;
        std::string Error;
    };

    struct RuntimeOverlayContext
    {
        nvrhi::ICommandList* CommandList = nullptr;
        nvrhi::IFramebuffer* Framebuffer = nullptr;
        nvrhi::ITexture* ColorTarget = nullptr;
        nvrhi::ITexture* StencilTarget = nullptr;
        nvrhi::ITexture* BackdropTexture = nullptr;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t FrameSlot = 0;
        uint64_t CompletedSubmission = 0;
    };

    // Called synchronously by SceneRenderer's graph-owned UI pass. Implementations
    // may record NVRHI commands, but never retain the frame-owned pointers.
    class IRuntimeOverlayRecorder
    {
    public:
        virtual ~IRuntimeOverlayRecorder() = default;
        [[nodiscard]] virtual bool HasDynamicSceneOverlay() const noexcept { return false; }
        [[nodiscard]] virtual RuntimeUiRenderMode GetSceneOverlayMode() const noexcept
        {
            return RuntimeUiRenderMode::Direct;
        }
        [[nodiscard]] virtual RuntimeUiTransitionParameters GetSceneOverlayTransition() const noexcept
        {
            return {};
        }
        virtual void RecordSceneOverlay(const RuntimeOverlayContext& context) = 0;
        virtual void NotifySceneOverlayFrameSubmitted(
            uint32_t frameSlot,
            uint64_t submission)
        {
            (void)frameSlot;
            (void)submission;
        }
        virtual void AbortSceneOverlayFrame(uint32_t frameSlot) noexcept
        {
            (void)frameSlot;
        }
        virtual void RecordWindowOverlay(const RuntimeOverlayContext& context) = 0;
    };

    struct RuntimeRendererFrameDesc
    {
        Ref<const RenderWorld> World;
        IRuntimeOverlayRecorder* OverlayRecorder = nullptr;
        std::filesystem::path CapturePath;
        FrameCaptureSource CaptureSource = FrameCaptureSource::Automatic;
        uint64_t ApplicationSimulationFrame = 0;
        bool bApplicationSimulationFrameValid = false;
        bool bCaptureSequenceHistory = false; // Bounded diagnostic only; no readback.
    };

    struct RuntimeRendererFrameResult
    {
        uint32_t FrameSlot = 0;
        uint64_t Submission = 0;
        TextureReadbackHandle Capture;
        std::string CaptureError;
        Json SequenceHistory = nullptr;
    };

	struct RuntimeOffscreenViewHandle
	{
		uint32_t Index = std::numeric_limits<uint32_t>::max();
		uint32_t Generation = 0;

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return Index != std::numeric_limits<uint32_t>::max() && Generation != 0;
		}

		[[nodiscard]] bool operator==(const RuntimeOffscreenViewHandle&) const = default;
	};

	struct RuntimeOffscreenOutput
	{
		nvrhi::TextureHandle Color;
		nvrhi::TextureHandle Depth;
		uint32_t Width = 0;
		uint32_t Height = 0;
		nvrhi::Format ColorFormat = nvrhi::Format::UNKNOWN;
		nvrhi::Format DepthFormat = nvrhi::Format::UNKNOWN;
		uint64_t Generation = 0;

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return Color && Depth && Width > 0 && Height > 0 && Generation > 0;
		}
	};

    // Owns the runtime RenderDevice, SceneRenderer and frame submission. This is
    // the only Application-facing owner of a graphics command list.
    class RuntimeRenderer final
    {
    public:
        RuntimeRenderer();
        ~RuntimeRenderer();

        RuntimeRenderer(const RuntimeRenderer&) = delete;
        RuntimeRenderer& operator=(const RuntimeRenderer&) = delete;

        void Initialize(const RuntimeRendererDesc& desc);
        void Shutdown();

        // Opens the host list before simulation; RenderFrame submits the same scope.
        void BeginSimulationFrame();
        void AbortSimulationFrame() noexcept;

        [[nodiscard]] RuntimeRendererFrameResult RenderFrame(
            const RuntimeRendererFrameDesc& desc,
            bool bVSync);
        [[nodiscard]] MaterialShaderRefreshResult RefreshMaterialShaderAtFrameBoundary(
            std::string_view sourcePath);
        void Resize(uint32_t width, uint32_t height);
        void WaitForIdle();
        void SetAntiAliasingSettings(const AntiAliasingSettings& settings);
        void ApplyRenderSettingsBatch(
            const ResolvedRenderSettings& settings,
            const RenderSettingsChangePlan& plan);
        [[nodiscard]] RuntimeConfigurationPreparationStats GetConfigurationPreparationStats() const noexcept;
        [[nodiscard]] AntiAliasingSettings GetAntiAliasingSettings() const;
        [[nodiscard]] AntiAliasingSettings
            GetRequestedAntiAliasingSettings() const;
        [[nodiscard]] AntiAliasingSettings
            GetEffectiveAntiAliasingSettings() const;
        [[nodiscard]] AntiAliasingCapabilities
            GetAntiAliasingCapabilities() const;
        [[nodiscard]] AntiAliasingSelection GetAntiAliasingSelection() const;
        void SetGlobalIlluminationSettings(
            const GlobalIlluminationSettings& settings);
        void ResetGlobalIlluminationHistory();
        void ResetTemporalHistory();
        [[nodiscard]] GlobalIlluminationSettings
            GetGlobalIlluminationSettings() const;
        [[nodiscard]] GlobalIlluminationSettings
            GetRequestedGlobalIlluminationSettings() const;
        [[nodiscard]] GlobalIlluminationSettings
            GetEffectiveGlobalIlluminationSettings() const;
        [[nodiscard]] GlobalIlluminationSelection
            GetGlobalIlluminationSelection() const;
        [[nodiscard]] std::vector<GlobalIlluminationTechniqueDescriptor>
            EnumerateGlobalIlluminationTechniques() const;
        [[nodiscard]] RenderCapabilities GetCapabilities() const;
        [[nodiscard]] Ref<const RenderWorld> GetLastSubmittedRenderWorld() const noexcept;
        void ActivateOfflineViewportRenderer(std::string providerId);
        void DeactivateOfflineViewportRenderer();
        [[nodiscard]] std::string GetActiveOfflineViewportRendererId() const;
        [[nodiscard]] IOfflineViewportRendererSession*
            GetOfflineViewportRendererSession() const noexcept;
        [[nodiscard]] std::vector<OfflineViewportRendererProviderDescriptor>
            EnumerateOfflineViewportRenderers() const;

        [[nodiscard]] RuntimeEnvironmentPreparation PrepareEnvironment(
            std::string_view path);
        [[nodiscard]] RuntimeEnvironmentPreparation GetEnvironmentPreparation(
            std::string_view path) const;
        [[nodiscard]] bool IsEnvironmentReady(std::string_view path) const;

		[[nodiscard]] ManagedTextureHandle LoadTexture(const TextureLoadDesc& desc);
		[[nodiscard]] const ManagedTexture& GetTexture(ManagedTextureHandle handle) const;
		void ReleaseTexture(ManagedTextureHandle handle, uint64_t retireSubmission);
		[[nodiscard]] size_t RequestTextureReload(std::string_view assetPath);
#if defined(HE_DEV_AGENT_RUNTIME)
        bool InjectUpscalingFailureForValidation(bool duringDispatch);
#endif

		void EnableMainOffscreenOutput();
		[[nodiscard]] RuntimeOffscreenOutput GetMainOffscreenOutput() const;
		[[nodiscard]] RuntimeOffscreenViewHandle CreateOffscreenView(
			uint32_t width,
			uint32_t height,
			std::string debugName);
		void ResizeOffscreenView(
			RuntimeOffscreenViewHandle handle,
			uint32_t width,
			uint32_t height);
		void SubmitOffscreenView(
			RuntimeOffscreenViewHandle handle,
			Ref<const RenderWorld> world);
		[[nodiscard]] RuntimeOffscreenOutput GetOffscreenOutput(
			RuntimeOffscreenViewHandle handle) const;
		[[nodiscard]] const SceneRendererStats& GetOffscreenSceneStats(RuntimeOffscreenViewHandle handle) const;
		[[nodiscard]] TextureReadbackHandle RequestOffscreenCapture(
			RuntimeOffscreenViewHandle handle,
			std::filesystem::path outputPath);
		void DestroyOffscreenView(RuntimeOffscreenViewHandle handle);

        void CollectCaptures(uint64_t completedSubmission);
        [[nodiscard]] bool IsCaptureValid(TextureReadbackHandle handle) const noexcept;
        [[nodiscard]] TextureReadbackResult GetCaptureResult(
            TextureReadbackHandle handle) const;
        void ReleaseCapture(TextureReadbackHandle handle);

        [[nodiscard]] bool IsInitialized() const noexcept;
        [[nodiscard]] nvrhi::IDevice* GetDevice() const;
        [[nodiscard]] RenderMemoryBudget QueryMemoryBudget() const;
        void BeginGpuTimingFrame(uint64_t cpuFrameId);
        void CancelGpuTimingFrame() noexcept;
        [[nodiscard]] std::vector<RenderSubmissionGpuTiming> TakeSubmissionTimings();
        [[nodiscard]] uint64_t GetLastTimingSampleId() const;

        [[nodiscard]] bool IsCaptureSourceAvailable(FrameCaptureSource source) const;
        [[nodiscard]] uint32_t GetFramesInFlight() const;
        [[nodiscard]] uint32_t GetWidth() const;
        [[nodiscard]] uint32_t GetHeight() const;
		[[nodiscard]] RenderBackend GetBackend() const;
		[[nodiscard]] std::string BuildSceneStatsSummary() const;
        [[nodiscard]] const SceneRendererStats& GetSceneStats() const;
		[[nodiscard]] RenderGraphDiagnosticSnapshot
			GetRenderGraphDiagnosticSnapshot() const;
		[[nodiscard]] std::string BuildRenderGraphDebugDump() const;

    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
