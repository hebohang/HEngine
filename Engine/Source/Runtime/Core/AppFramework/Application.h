#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Core/Templates/PublicSingleton.h"
#include "Runtime/Core/Window/Window.h"
#include "Runtime/Core/AppFramework/Layer/LayerStack.h"
#include "Runtime/Core/Timestep.h"
#include "Runtime/Events/Event.h"
#include "Runtime/Events/ApplicationEvent.h"
#include "Runtime/Graphics/Gui/NvrhiImGuiRenderer.h"
#include "Runtime/Graphics/RHI/RenderBackend.h"
#include "Runtime/ImGui/ImGuiLayer.h"
#include "Runtime/Graphics/RHI/TextureReadback.h"
#include "Runtime/Graphics/RHI/FrameCaptureSource.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

int main(int argc, char** argv);

namespace HEngine
{
	class NvrhiImGuiLayer;
	class RuntimeRenderer;
	struct SceneRendererStats;
    struct RenderMemoryBudget;
	class RuntimeProjectResourceWarmup;
	struct RuntimeUiTextureRegistry;
	struct RenderWorld;
#if defined(HE_DEV_AGENT_RUNTIME)
	struct ManagedTextureHandle;
#endif
	struct RuntimeOffscreenOutput;
	struct RuntimeOffscreenViewHandle;
	struct RuntimeEnvironmentPreparation;
    struct RuntimeConfigurationPreparationStats;
	struct MaterialShaderRefreshResult;
	struct AntiAliasingSettings;
	struct AntiAliasingSelection;
	struct AntiAliasingCapabilities;
	struct GlobalIlluminationSettings;
	struct GlobalIlluminationSelection;
	struct GlobalIlluminationTechniqueDescriptor;
	struct OfflineViewportRendererProviderDescriptor;
	class IOfflineViewportRendererSession;
	struct RenderCapabilities;
	struct ResolvedRenderSettings;
	struct RenderSettingsDraft;
	struct RenderSettingsApplyResult;
	struct RenderGraphDiagnosticSnapshot;

	enum class RuntimeFrameCaptureState : std::uint8_t
	{
		Idle,
		Queued,
		InFlight,
		Complete,
		Failed,
	};

	struct RuntimeFrameCaptureProgress
	{
		RuntimeFrameCaptureState State = RuntimeFrameCaptureState::Idle;
		std::filesystem::path OutputPath;
        FrameCaptureSource Source = FrameCaptureSource::Automatic;
		std::string Error;
		std::uint64_t RenderFrame = 0;
		std::uint64_t Submission = 0;
        std::uint64_t RenderWorldFrame = 0; // Extraction sequence, not SimulationClock.
		double SimulationTime = 0.0;
        std::uint64_t SimulationStep = 0;
	};

	struct ApplicationRunOptions
	{
		std::uint64_t ExitAfterFrames = 0;
		std::uint64_t CaptureFrame = 0;
		float FixedDeltaSeconds = 0.0f;
		std::uint32_t WindowWidth = 0;
		std::uint32_t WindowHeight = 0;
        std::optional<bool> FullscreenOverride;
		std::filesystem::path CapturePath;
		std::filesystem::path ProjectRoot;
		std::string ProjectGameConfig;
		std::filesystem::path SavedRoot;
		std::filesystem::path RenderStatsPath;
		std::filesystem::path RenderSettingsPath;
		std::uint64_t PerformanceWarmupFrames = 30;
        std::string StartupScene;
        std::string StartupViewportMode;
		std::string PackageCheckId;
		std::optional<RenderBackend> GraphicsApiOverride;
		std::optional<std::string> RenderingPathOverride;
#if defined(HE_DEV_AGENT_RUNTIME)
		std::filesystem::path AgentSessionDirectory;
		std::filesystem::path InputReplayPath;
		std::string AgentToken;
#endif
		bool bNoCursorCapture = false;
		bool bNoVSync = false;
		bool bUseWarp = false;
		bool bStrictGraphicsApi = false;
		bool bRenderValidation = false;
		bool bRenderStats = false;
#if defined(HE_DEV_AGENT_RUNTIME)
		bool bAgentStartPaused = false;
#endif

		[[nodiscard]] bool HasFixedWindowSize() const
		{
			return WindowWidth > 0 && WindowHeight > 0;
		}

		[[nodiscard]] bool HasFrameCapture() const
		{
			return CaptureFrame > 0 && !CapturePath.empty();
		}

#if defined(HE_DEV_AGENT_RUNTIME)
		[[nodiscard]] bool HasAgentSession() const
		{
			return !AgentSessionDirectory.empty();
		}
#endif
	};

#if defined(HE_DEV_AGENT_RUNTIME)
    struct TextureMipReadbackProbe
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint64_t Hash = 0;
        uint32_t CoveredPixels = 0;
    };

    struct TextureReadyProbeResult
    {
        bool SharedHandle = false;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t MipLevels = 0;
        std::string FormatName;
        std::vector<TextureMipReadbackProbe> MipReadback;
    };
    struct TextureBurstProbeResult
    {
        uint32_t Requested = 0;
        uint32_t Ready = 0;
        uint32_t Shared = 0;
    };
    struct TextureStreamingProbeResult
    {
        uint32_t Requested = 0;
        uint32_t Ready = 0;
        uint32_t Pending = 0;
    };
#endif

	class Application : public PublicSingleton<Application>
	{
		friend int ::main(int argc, char** argv);
		// To be defined in CLIENT
		friend void MyAppInitialize(Application& app);
	public:
		Application();
		virtual ~Application();

		void OnEvent(Event& e);

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* layer);
		void PopLayer(Layer* layer);

		[[nodiscard]] static Window& GetWindow() { return *(Application::GetInstance().mWindow); }
		// Layers explicitly submit immutable snapshots after ECS extraction. The
		// Application never reaches into a Level or registry to obtain render data.
		void SubmitRenderWorld(Ref<const RenderWorld> world);
		[[nodiscard]] Ref<const RenderWorld> GetLastSubmittedRenderWorld() const noexcept;
        [[nodiscard]] RuntimeConfigurationPreparationStats GetConfigurationPreparationStats() const noexcept;
		[[nodiscard]] MaterialShaderRefreshResult RefreshMaterialShaderAtFrameBoundary(
			std::string_view sourcePath);
		[[nodiscard]] RuntimeEnvironmentPreparation PrepareRenderEnvironment(
			std::string_view path);
		[[nodiscard]] std::uint32_t GetRenderWidth() const;
		[[nodiscard]] std::uint32_t GetRenderHeight() const;
		void ApplyAntiAliasingSettings(const AntiAliasingSettings& settings);
		[[nodiscard]] AntiAliasingSettings GetAntiAliasingSettings() const;
		[[nodiscard]] AntiAliasingSettings
			GetRequestedAntiAliasingSettings() const;
		[[nodiscard]] AntiAliasingSettings
			GetEffectiveAntiAliasingSettings() const;
		[[nodiscard]] AntiAliasingCapabilities
			GetAntiAliasingCapabilities() const;
		[[nodiscard]] AntiAliasingSelection GetAntiAliasingSelection() const;
		void ApplyGlobalIlluminationSettings(
			const GlobalIlluminationSettings& settings);
		void ResetGlobalIlluminationHistory();
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
		[[nodiscard]] RenderCapabilities GetRenderCapabilities() const;
		[[nodiscard]] ResolvedRenderSettings GetResolvedRenderSettings() const;
		[[nodiscard]] RenderSettingsApplyResult ApplyRenderSettingsDraft(
			const RenderSettingsDraft& draft,
			bool bPersist = true);
		void ActivateOfflineViewportRenderer(std::string providerId);
		void DeactivateOfflineViewportRenderer();
		[[nodiscard]] std::string GetActiveOfflineViewportRendererId() const;
		[[nodiscard]] IOfflineViewportRendererSession*
			GetOfflineViewportRendererSession() const noexcept;
		[[nodiscard]] std::vector<OfflineViewportRendererProviderDescriptor>
			EnumerateOfflineViewportRenderers() const;
		[[nodiscard]] RenderGraphDiagnosticSnapshot
			GetRenderGraphDiagnosticSnapshot() const;
		[[nodiscard]] std::string BuildRenderGraphDebugDump() const;
		[[nodiscard]] ImGuiTextureHandle LoadImGuiTexture(
			const std::filesystem::path& path,
			bool bSrgb = true,
			uint32_t maximumDimension = 0);
		void ReleaseImGuiTexture(ImGuiTextureHandle handle);
		[[nodiscard]] size_t RequestTextureReload(std::string_view assetPath);
#if defined(HE_DEV_AGENT_RUNTIME)
        // One-shot failure for bounded Agent validation; absent from shipping.
        bool InjectUpscalingFailureForValidation(bool duringDispatch);
		// Agent validation only: request an asset asynchronously, then require it
		// ready through the same cache key and release both temporary leases.
		[[nodiscard]] TextureReadyProbeResult ProbeTextureReady(
			std::string_view assetPath, uint32_t maximumDimension,
			bool bSrgb, bool bGenerateMipmaps,
			bool bPreserveAlphaCoverage, float alphaCoverageCutoff,
			bool bReadbackMips = false);
		[[nodiscard]] TextureBurstProbeResult ProbeTextureBurst(
			std::string_view assetPath, uint32_t count);
		[[nodiscard]] TextureStreamingProbeResult BeginTextureStreamingProbe(
			std::string_view assetPath, uint32_t count,
			uint32_t baseDimension);
		[[nodiscard]] TextureStreamingProbeResult InspectTextureStreamingProbe(
			bool bRelease);
#endif
		[[nodiscard]] RuntimeOffscreenViewHandle CreateOffscreenView(
			std::uint32_t width,
			std::uint32_t height,
			std::string debugName);
		void ResizeOffscreenView(
			RuntimeOffscreenViewHandle handle,
			std::uint32_t width,
			std::uint32_t height);
		void SubmitOffscreenView(
			RuntimeOffscreenViewHandle handle,
			Ref<const RenderWorld> world);
		[[nodiscard]] RuntimeOffscreenOutput GetOffscreenOutput(
			RuntimeOffscreenViewHandle handle) const;
		[[nodiscard]] const SceneRendererStats& GetOffscreenSceneStats(RuntimeOffscreenViewHandle handle) const;
        [[nodiscard]] const SceneRendererStats& GetMainSceneStats() const;
        [[nodiscard]] RenderMemoryBudget GetRenderMemoryBudget() const;
		[[nodiscard]] TextureReadbackHandle RequestOffscreenCapture(
			RuntimeOffscreenViewHandle handle,
			std::filesystem::path outputPath);
		[[nodiscard]] bool IsOffscreenCaptureValid(TextureReadbackHandle handle) const noexcept;
		[[nodiscard]] TextureReadbackResult GetOffscreenCaptureResult(
			TextureReadbackHandle handle) const;
		void ReleaseOffscreenCapture(TextureReadbackHandle handle);
		void DestroyOffscreenView(RuntimeOffscreenViewHandle handle);
		[[nodiscard]] ImGuiTextureHandle RegisterImGuiTexture(
			const nvrhi::TextureHandle& texture,
			nvrhi::Format format = nvrhi::Format::UNKNOWN);
		void UnregisterImGuiTexture(ImGuiTextureHandle handle);
		void EnableMainOffscreenOutput();
		[[nodiscard]] RuntimeOffscreenOutput GetMainOffscreenOutput() const;
		[[nodiscard]] bool RequestFrameCapture(std::filesystem::path outputPath,
            FrameCaptureSource source = FrameCaptureSource::Automatic);
        [[nodiscard]] bool RequestFrameCaptureSequence(std::vector<std::filesystem::path> paths, bool bCameraCut = false, uint32_t settleFrames = 0,
            FrameCaptureSource source = FrameCaptureSource::Automatic);
        [[nodiscard]] bool CanAdvanceCaptureSequence() const;
        [[nodiscard]] const Json& GetCaptureSequenceHistory() const { return mSequenceHistory; }
        [[nodiscard]] const std::vector<RuntimeFrameCaptureProgress>& GetFrameCaptureSequence() const { return mSequenceCaptures; }
        void CancelFrameCaptureSequence();
        void SetCaptureSimulationStep(std::uint64_t step) { mCaptureSimulationStep = step; }
        [[nodiscard]] bool IsFrameCaptureSourceAvailable(FrameCaptureSource source) const;
		[[nodiscard]] const RuntimeFrameCaptureProgress& GetFrameCaptureProgress() const
		{
			return mFrameCaptureProgress;
		}
		void ClearFrameCapture();

		void Close();
		void RequestRestart();
		[[nodiscard]] bool IsRestartRequested() const noexcept
		{
			return bRestartRequested;
		}
		void BeginRuntimeStartup();
		void NotifyRuntimeStartupPresentedFrame();
		void RequestRuntimeStartupGameplayPresent();
		void NotifyRuntimeStartupGameplayPresentedFrame();
		void CompleteRuntimeStartup();
		void CancelRuntimeStartup() noexcept;
		// Runtime scripts enqueue scene changes while their current Level is still
		// executing. GameLayer consumes the request at the next safe frame boundary.
		[[nodiscard]] bool RequestRuntimeScene(std::string scenePath, bool bForceReload = false);
		[[nodiscard]] std::optional<std::string> ConsumeRuntimeSceneRequest(bool* forceReload = nullptr);
		void ClearRuntimeSceneRequest() noexcept;
		[[nodiscard]] bool HasRuntimeSceneRequest() const noexcept
		{
			return mRuntimeSceneRequest.has_value();
		}

		[[nodiscard]] ImGuiLayer* GetImGuiLayer() { return mImGuiLayer; }
		[[nodiscard]] RuntimeProjectResourceWarmup& GetProjectResourceWarmup();
		[[nodiscard]] RenderBackend GetRenderBackend() const;
		[[nodiscard]] const ApplicationRunOptions& GetRunOptions() const { return mRunOptions; }
		[[nodiscard]] std::uint64_t GetFrameNumber() const;
		[[nodiscard]] std::uint64_t GetCompletedFrameCount() const;
		[[nodiscard]] double GetSimulationTimeSeconds() const;
		[[nodiscard]] bool IsMinimized() const { return bMinimized; }
		[[nodiscard]] bool IsRuntimeStartupPending() const noexcept { return bRuntimeStartupPending; }
        void SetRuntimeSceneLoading(bool bLoading) noexcept { bRuntimeSceneLoading = bLoading; }
        [[nodiscard]] bool IsRuntimeSceneLoading() const noexcept
        { return bRuntimeStartupPending || bRuntimeSceneLoading; }
		[[nodiscard]] bool HasRuntimeStartupPresentedFrame() const noexcept
		{
			return bRuntimeStartupPending && bRuntimeStartupFirstPresented;
		}
		[[nodiscard]] bool HasRuntimeStartupGameplayPresentedFrame() const noexcept
		{
			return bRuntimeStartupPending && bRuntimeStartupGameplayPresented;
		}
	private:
		void Init(const std::string& name);
		void ConfigureRunOptions(ApplicationRunOptions options) { mRunOptions = std::move(options); }
		void Run();
		void Clean();
		bool OnWindowClose(WindowCloseEvent& e);
		bool OnWindowResize(WindowResizeEvent& e);
		void PollFrameCapture();
	private:
		Scope<Window> mWindow;
		Scope<RuntimeRenderer> mRuntimeRenderer;
		Ref<const RenderWorld> mRenderWorld;
		Scope<RuntimeProjectResourceWarmup> mProjectResourceWarmup;
		Scope<RuntimeUiTextureRegistry> mUiTextureRegistry;
		ImGuiLayer* mImGuiLayer = nullptr;
		NvrhiImGuiLayer* mNvrhiImGuiLayer = nullptr;
		TextureReadbackHandle mFrameCaptureHandle;
		RuntimeFrameCaptureProgress mFrameCaptureProgress;
        std::vector<RuntimeFrameCaptureProgress> mSequenceCaptures;
        bool bSequenceCameraCut = false;
        uint32_t mSequenceSettleRemaining = 0;
        uint64_t mSequenceSettleSimulationStep = 0;
        Json mSequenceHistory = Json::array();
        std::vector<TextureReadbackHandle> mSequenceHandles;
        size_t mSequenceRecorded = 0;
        std::uint64_t mCaptureSimulationStep = 0;
		std::uint32_t mLastFrameSlot = 0;
		std::uint64_t mLastSubmission = 0;
#if defined(HE_DEV_AGENT_RUNTIME)
		std::vector<ManagedTextureHandle> mTextureStreamingProbe;
#endif
		bool bRunning = true;
		bool bMinimized = false;
		bool bCleaned = true;
		bool bRestartRequested = false;
		bool bRuntimeStartupPending = false;
        bool bRuntimeSceneLoading = false;
		bool bRuntimeStartupFirstPresented = false;
		bool bRuntimeStartupGameplayPresentRequested = false;
		bool bRuntimeStartupGameplayPresented = false;
		std::optional<std::string> mRuntimeSceneRequest;
		bool bForceRuntimeSceneReload = false;
		LayerStack mLayerStack;
		ApplicationRunOptions mRunOptions;
	};
}



