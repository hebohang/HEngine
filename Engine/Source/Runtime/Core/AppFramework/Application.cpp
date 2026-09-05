#include "Hepch.h"
#include "Application.h"

#include "Runtime/Audio/AudioEngine.h"
#include "Runtime/Core/Input/Input.h"
#include "Runtime/Core/PresentationClock.h"
#include "Runtime/Core/SimulationClock.h"
#include "Runtime/Graphics/Material/MaterialShaderManager.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIlluminationSettingsJson.h"
#include "Runtime/Graphics/RHI/RenderSurface.h"
#include "Runtime/Graphics/Renderer/AntiAliasing.h"
#include "Runtime/Graphics/Renderer/RuntimeRenderer.h"
#include "Runtime/Graphics/Settings/RenderSettingsService.h"
#include "Runtime/ImGui/NvrhiImGuiLayer.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/RuntimeProjectResourceWarmup.h"
#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceCatalog.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeResourceRegistry.h"
#include "Runtime/Core/Meta/Reflection/ReflectionRegister.h"
#include "Runtime/Core/Modules/ModuleManager.h"
#include "Runtime/UI/Rml/RmlUiManager.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

#if HE_ENABLE_GM
#include "Runtime/DevTools/GM/GMPanelService.h"
#endif

#include <HEngine/EngineVersion.h>

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace HEngine
{
	struct RuntimeUiTextureRegistry
	{
		std::unordered_map<std::uint64_t, ManagedTextureHandle> Registrations;
		std::unordered_map<std::uint64_t, ImGuiTextureHandle> ExternalRegistrations;
	};

	namespace
	{
		[[nodiscard]] std::uint64_t MakeImGuiTextureKey(ImGuiTextureHandle handle) noexcept
		{
			return (static_cast<std::uint64_t>(handle.Generation) << 32u) |
				static_cast<std::uint64_t>(handle.Index);
		}

		class RuntimeUiOverlayRecorder final : public IRuntimeOverlayRecorder
		{
		public:
			RuntimeUiOverlayRecorder(NvrhiImGuiLayer& imguiLayer, RmlUiManager& rmlUi)
				: mImGuiLayer(imguiLayer), mRmlUi(rmlUi)
			{
			}

            [[nodiscard]] bool HasDynamicSceneOverlay() const noexcept override
            {
                return mRmlUi.HasActiveDocument();
            }

            [[nodiscard]] RuntimeUiRenderMode GetSceneOverlayMode() const noexcept override
            {
                return mRmlUi.HasActiveDocument()
                    ? RuntimeUiRenderMode::Offscreen
                    : RuntimeUiRenderMode::Direct;
            }

            [[nodiscard]] RuntimeUiTransitionParameters GetSceneOverlayTransition() const noexcept override
            {
                return mRmlUi.GetRuntimeUiTransition();
            }

			void RecordSceneOverlay(const RuntimeOverlayContext& context) override
			{
				try
				{
					if (mRmlUi.IsInitialized())
					{
						mRmlUi.BeginGpuFrame(
							context.CommandList,
							context.Framebuffer,
							context.FrameSlot,
							context.CompletedSubmission,
                            context.BackdropTexture);
						mRmlUi.Render();
						mRmlUi.EndGpuFrame();
					}
				}
				catch (...)
				{
					if (mRmlUi.IsGpuFrameActive())
					{
						try
						{
							mRmlUi.EndGpuFrame();
						}
						catch (...)
						{
							Log::WriteFallbackError(
								"RmlUi GPU frame cleanup failed after overlay recording.",
								"EndGpuFrame raised an exception");
						}
					}
					throw;
				}
			}

			void RecordWindowOverlay(const RuntimeOverlayContext& context) override
			{
				mImGuiLayer.RecordGpuFrame(
					context.CommandList,
					context.Framebuffer,
					context.FrameSlot);
			}

            void NotifySceneOverlayFrameSubmitted(
                uint32_t frameSlot,
                uint64_t submission) override
            {
                mRmlUi.NotifyGpuFrameSubmitted(frameSlot, submission);
            }

            void AbortSceneOverlayFrame(uint32_t frameSlot) noexcept override
            {
                mRmlUi.AbortGpuFrame(frameSlot);
            }

		private:
			NvrhiImGuiLayer& mImGuiLayer;
			RmlUiManager& mRmlUi;
		};
	}

	Application::Application() = default;

	Application::~Application() = default;

	bool Application::RequestRuntimeScene(std::string scenePath)
	{
		if (scenePath.empty() || mRuntimeSceneRequest)
			return false;
		mRuntimeSceneRequest = std::move(scenePath);
		return true;
	}

	std::optional<std::string> Application::ConsumeRuntimeSceneRequest()
	{
		std::optional<std::string> request = std::move(mRuntimeSceneRequest);
		mRuntimeSceneRequest.reset();
		return request;
	}

	void Application::ClearRuntimeSceneRequest() noexcept
	{
		mRuntimeSceneRequest.reset();
	}

	void Application::SubmitRenderWorld(Ref<const RenderWorld> world)
	{
		mRenderWorld = std::move(world);
	}

	MaterialShaderRefreshResult Application::RefreshMaterialShaderAtFrameBoundary(
		std::string_view sourcePath)
	{
		if (!mRuntimeRenderer)
			throw std::logic_error(
				"Application material shader refresh requires RuntimeRenderer");
		return mRuntimeRenderer->RefreshMaterialShaderAtFrameBoundary(sourcePath);
	}

	RuntimeEnvironmentPreparation Application::PrepareRenderEnvironment(
		std::string_view path)
	{
		if (!mRuntimeRenderer)
			throw std::logic_error("Application render environment preparation requires RuntimeRenderer");
		return mRuntimeRenderer->PrepareEnvironment(path);
	}

	std::uint32_t Application::GetRenderWidth() const
	{
		return mRuntimeRenderer ? mRuntimeRenderer->GetWidth() : 0;
	}

	std::uint32_t Application::GetRenderHeight() const
	{
		return mRuntimeRenderer ? mRuntimeRenderer->GetHeight() : 0;
	}

	void Application::ApplyAntiAliasingSettings(
		const AntiAliasingSettings& settings)
	{
		RenderSettingsService& service = RenderSettingsService::GetInstance();
		const ResolvedRenderSettings snapshot = service.GetSnapshot();
		RenderSettings requested = snapshot.Requested;
		requested.AntiAliasing = settings;
		const Json values = RenderSettingsToFlatValues(requested);
		RenderSettingsDraft draft = service.CreateDraft(SettingsLayer::Session);
		for (const auto& [id, value] : values.items())
		{
			if (std::string_view(id).starts_with("Rendering.AntiAliasing."))
				draft.Patch[id] = value;
		}
		draft.Patch[std::string(RenderSettingIds::CaptureSupersampleScale)] =
			settings.CaptureSupersampleScale;
		const RenderSettingsApplyResult result = service.ApplyDraft(draft, false);
		if (!result.bSuccess)
			throw std::runtime_error(result.Message);
	}

	AntiAliasingSettings Application::GetAntiAliasingSettings() const
	{
		return GetRequestedAntiAliasingSettings();
	}

	AntiAliasingSettings
		Application::GetRequestedAntiAliasingSettings() const
	{
		return RenderSettingsService::GetInstance()
			.BuildRequestedRuntimeAntiAliasing();
	}

	AntiAliasingSettings
		Application::GetEffectiveAntiAliasingSettings() const
	{
		return mRuntimeRenderer
			? mRuntimeRenderer->GetEffectiveAntiAliasingSettings()
			: AntiAliasingSettings{};
	}

	AntiAliasingCapabilities
		Application::GetAntiAliasingCapabilities() const
	{
		return mRuntimeRenderer
			? mRuntimeRenderer->GetAntiAliasingCapabilities()
			: AntiAliasingCapabilities{};
	}

	AntiAliasingSelection Application::GetAntiAliasingSelection() const
	{
		return mRuntimeRenderer
			? mRuntimeRenderer->GetAntiAliasingSelection()
			: AntiAliasingSelection{};
	}

	void Application::ApplyGlobalIlluminationSettings(
		const GlobalIlluminationSettings& settings)
	{
		RenderSettingsService& service = RenderSettingsService::GetInstance();
		const ResolvedRenderSettings snapshot = service.GetSnapshot();
		RenderSettings requested = snapshot.Requested;
		requested.GlobalIllumination = settings;
		const Json values = RenderSettingsToFlatValues(requested);
		RenderSettingsDraft draft = service.CreateDraft(SettingsLayer::Session);
		for (const auto& [id, value] : values.items())
		{
			if (std::string_view(id).starts_with("Rendering.GI."))
				draft.Patch[id] = value;
		}
		draft.Patch[std::string(RenderSettingIds::GiDebugView)] =
			static_cast<std::uint32_t>(settings.DebugView);
		draft.Patch[std::string(RenderSettingIds::GiDirectOnly)] =
			settings.bDirectOnly;
		const RenderSettingsApplyResult result = service.ApplyDraft(draft, false);
		if (!result.bSuccess)
			throw std::runtime_error(result.Message);
	}

	void Application::ResetGlobalIlluminationHistory()
	{
		if (mRuntimeRenderer)
			mRuntimeRenderer->ResetGlobalIlluminationHistory();
	}

	GlobalIlluminationSettings Application::GetGlobalIlluminationSettings() const
	{
		return GetRequestedGlobalIlluminationSettings();
	}

	GlobalIlluminationSettings
		Application::GetRequestedGlobalIlluminationSettings() const
	{
		return RenderSettingsService::GetInstance()
			.BuildRequestedRuntimeGlobalIllumination();
	}

	GlobalIlluminationSettings
		Application::GetEffectiveGlobalIlluminationSettings() const
	{
		return mRuntimeRenderer
			? mRuntimeRenderer->GetEffectiveGlobalIlluminationSettings()
			: GlobalIlluminationSettings{};
	}

	GlobalIlluminationSelection Application::GetGlobalIlluminationSelection() const
	{
		return mRuntimeRenderer
			? mRuntimeRenderer->GetGlobalIlluminationSelection()
			: GlobalIlluminationSelection{};
	}

	std::vector<GlobalIlluminationTechniqueDescriptor>
		Application::EnumerateGlobalIlluminationTechniques() const
	{
		return mRuntimeRenderer
			? mRuntimeRenderer->EnumerateGlobalIlluminationTechniques()
			: std::vector<GlobalIlluminationTechniqueDescriptor>{};
	}

	RenderCapabilities Application::GetRenderCapabilities() const
	{
		return mRuntimeRenderer
			? mRuntimeRenderer->GetCapabilities()
			: RenderCapabilities{};
	}

	ResolvedRenderSettings Application::GetResolvedRenderSettings() const
	{
		return RenderSettingsService::GetInstance().GetSnapshot();
	}

	RenderSettingsApplyResult Application::ApplyRenderSettingsDraft(
		const RenderSettingsDraft& draft,
		bool bPersist)
	{
		return RenderSettingsService::GetInstance().ApplyDraft(draft, bPersist);
	}

	void Application::ActivateOfflineViewportRenderer(std::string providerId)
	{
		if (!mRuntimeRenderer)
			throw std::logic_error(
				"Offline viewport renderer activation requires RuntimeRenderer");
		mRuntimeRenderer->ActivateOfflineViewportRenderer(std::move(providerId));
		RenderSettingsCapabilities capabilities =
			RenderSettingsService::GetInstance().GetSnapshot().Capabilities;
		capabilities.AntiAliasingPath =
			AntiAliasingRenderPath::OfflinePathTracing;
		capabilities.bExternalViewportRendererActive = true;
		RenderSettingsService::GetInstance().SetCapabilities(
			std::move(capabilities));
	}

	void Application::DeactivateOfflineViewportRenderer()
	{
		if (mRuntimeRenderer)
			mRuntimeRenderer->DeactivateOfflineViewportRenderer();
		RenderSettingsCapabilities capabilities =
			RenderSettingsService::GetInstance().GetSnapshot().Capabilities;
		capabilities.AntiAliasingPath =
			AntiAliasingRenderPath::RealtimeRaster;
		capabilities.bExternalViewportRendererActive = false;
		RenderSettingsService::GetInstance().SetCapabilities(
			std::move(capabilities));
	}

	std::string Application::GetActiveOfflineViewportRendererId() const
	{
		return mRuntimeRenderer
			? mRuntimeRenderer->GetActiveOfflineViewportRendererId()
			: std::string{};
	}

	IOfflineViewportRendererSession*
		Application::GetOfflineViewportRendererSession() const noexcept
	{
		return mRuntimeRenderer
			? mRuntimeRenderer->GetOfflineViewportRendererSession()
			: nullptr;
	}

	std::vector<OfflineViewportRendererProviderDescriptor>
		Application::EnumerateOfflineViewportRenderers() const
	{
		return mRuntimeRenderer
			? mRuntimeRenderer->EnumerateOfflineViewportRenderers()
			: std::vector<OfflineViewportRendererProviderDescriptor>{};
	}

	RenderGraphDiagnosticSnapshot
		Application::GetRenderGraphDiagnosticSnapshot() const
	{
		return mRuntimeRenderer
			? mRuntimeRenderer->GetRenderGraphDiagnosticSnapshot()
			: RenderGraphDiagnosticSnapshot{};
	}

	std::string Application::BuildRenderGraphDebugDump() const
	{
		return mRuntimeRenderer
			? mRuntimeRenderer->BuildRenderGraphDebugDump()
			: std::string{};
	}

	ImGuiTextureHandle Application::LoadImGuiTexture(
		const std::filesystem::path& path,
		bool bSrgb,
		uint32_t maximumDimension)
	{
		if (!mRuntimeRenderer || !mNvrhiImGuiLayer || path.empty())
			throw std::logic_error("ImGui texture loading requires an initialized runtime renderer");
		if (!mUiTextureRegistry)
			mUiTextureRegistry = CreateScope<RuntimeUiTextureRegistry>();

		const ManagedTextureHandle managed = mRuntimeRenderer->LoadTexture(TextureLoadDesc{
			.AssetPath = Utils::PathToUtf8(path),
			.ColorSpace = bSrgb ? TextureColorSpace::SRgb : TextureColorSpace::Linear,
			// ImGui registers its own SRV below. Allocating a second material-table
			// descriptor here can exhaust the bounded baseline table before scene
			// materials (for example late Sponza slots) are loaded.
			.bCreateBindlessDescriptor = false,
			.bAllowAbsolutePath = path.is_absolute(),
			.bGenerateMipmaps = false,
			.MaximumDimension = maximumDimension,
			.DebugName = "ImGui Asset " + Utils::PathToUtf8(path.filename()),
		});
		try
		{
			const ManagedTexture& texture = mRuntimeRenderer->GetTexture(managed);
			const ImGuiTextureHandle imgui = mNvrhiImGuiLayer->RegisterTexture(
				texture.Texture,
				nullptr,
				texture.Format);
			mUiTextureRegistry->Registrations.emplace(MakeImGuiTextureKey(imgui), managed);
			return imgui;
		}
		catch (...)
		{
			mRuntimeRenderer->ReleaseTexture(managed, mLastSubmission);
			throw;
		}
	}

	void Application::ReleaseImGuiTexture(ImGuiTextureHandle handle)
	{
		if (!handle || !mUiTextureRegistry)
			return;
		const auto registration =
			mUiTextureRegistry->Registrations.find(MakeImGuiTextureKey(handle));
		if (registration == mUiTextureRegistry->Registrations.end())
			return;
		if (mNvrhiImGuiLayer && mNvrhiImGuiLayer->IsTextureValid(handle))
			mNvrhiImGuiLayer->UnregisterTexture(handle, mLastFrameSlot);
		if (mRuntimeRenderer && mRuntimeRenderer->IsInitialized())
			mRuntimeRenderer->ReleaseTexture(registration->second, mLastSubmission);
		mUiTextureRegistry->Registrations.erase(registration);
	}

	RuntimeOffscreenViewHandle Application::CreateOffscreenView(
		std::uint32_t width,
		std::uint32_t height,
		std::string debugName)
	{
		return mRuntimeRenderer->CreateOffscreenView(
			width,
			height,
			std::move(debugName));
	}

	void Application::ResizeOffscreenView(
		RuntimeOffscreenViewHandle handle,
		std::uint32_t width,
		std::uint32_t height)
	{
		mRuntimeRenderer->ResizeOffscreenView(handle, width, height);
	}

	void Application::SubmitOffscreenView(
		RuntimeOffscreenViewHandle handle,
		Ref<const RenderWorld> world)
	{
		mRuntimeRenderer->SubmitOffscreenView(handle, std::move(world));
	}

	RuntimeOffscreenOutput Application::GetOffscreenOutput(
		RuntimeOffscreenViewHandle handle) const
	{
		return mRuntimeRenderer->GetOffscreenOutput(handle);
	}

	TextureReadbackHandle Application::RequestOffscreenCapture(
		RuntimeOffscreenViewHandle handle,
		std::filesystem::path outputPath)
	{
		if (!mRuntimeRenderer)
			throw std::logic_error("Offscreen capture requires RuntimeRenderer");
		return mRuntimeRenderer->RequestOffscreenCapture(handle, std::move(outputPath));
	}

	bool Application::IsOffscreenCaptureValid(TextureReadbackHandle handle) const noexcept
	{
		return mRuntimeRenderer && mRuntimeRenderer->IsCaptureValid(handle);
	}

	TextureReadbackResult Application::GetOffscreenCaptureResult(
		TextureReadbackHandle handle) const
	{
		if (!mRuntimeRenderer)
			throw std::logic_error("Offscreen capture query requires RuntimeRenderer");
		return mRuntimeRenderer->GetCaptureResult(handle);
	}

	void Application::ReleaseOffscreenCapture(TextureReadbackHandle handle)
	{
		if (!mRuntimeRenderer)
			throw std::logic_error("Offscreen capture release requires RuntimeRenderer");
		mRuntimeRenderer->ReleaseCapture(handle);
	}

	void Application::DestroyOffscreenView(RuntimeOffscreenViewHandle handle)
	{
		if (mRuntimeRenderer)
			mRuntimeRenderer->DestroyOffscreenView(handle);
	}

	ImGuiTextureHandle Application::RegisterImGuiTexture(
		const nvrhi::TextureHandle& texture,
		nvrhi::Format format)
	{
		if (!mNvrhiImGuiLayer || !texture)
			throw std::invalid_argument("ImGui texture registration requires a texture");
		if (!mUiTextureRegistry)
			mUiTextureRegistry = CreateScope<RuntimeUiTextureRegistry>();
		const ImGuiTextureHandle handle =
			mNvrhiImGuiLayer->RegisterTexture(texture, nullptr, format);
		mUiTextureRegistry->ExternalRegistrations.emplace(
			MakeImGuiTextureKey(handle),
			handle);
		return handle;
	}

	void Application::UnregisterImGuiTexture(ImGuiTextureHandle handle)
	{
		if (!handle || !mUiTextureRegistry)
			return;
		const auto registration = mUiTextureRegistry->ExternalRegistrations.find(
			MakeImGuiTextureKey(handle));
		if (registration == mUiTextureRegistry->ExternalRegistrations.end())
			return;
		if (mNvrhiImGuiLayer && mNvrhiImGuiLayer->IsTextureValid(handle))
			mNvrhiImGuiLayer->UnregisterTexture(handle, mLastFrameSlot);
		mUiTextureRegistry->ExternalRegistrations.erase(registration);
	}

	void Application::EnableMainOffscreenOutput()
	{
		mRuntimeRenderer->EnableMainOffscreenOutput();
	}

	RuntimeOffscreenOutput Application::GetMainOffscreenOutput() const
	{
		return mRuntimeRenderer->GetMainOffscreenOutput();
	}

	bool Application::RequestFrameCapture(std::filesystem::path outputPath)
	{
		if (!mRuntimeRenderer || outputPath.empty() ||
			mFrameCaptureProgress.State != RuntimeFrameCaptureState::Idle)
		{
			return false;
		}

		mFrameCaptureProgress = {
			.State = RuntimeFrameCaptureState::Queued,
			.OutputPath = std::move(outputPath),
		};
		return true;
	}

	void Application::ClearFrameCapture()
	{
		if (mFrameCaptureProgress.State == RuntimeFrameCaptureState::Queued ||
			mFrameCaptureProgress.State == RuntimeFrameCaptureState::InFlight)
		{
			throw std::logic_error("Cannot clear an unfinished runtime frame capture");
		}
		mFrameCaptureHandle = {};
		mFrameCaptureProgress = {};
	}

	void Application::PollFrameCapture()
	{
		if (mFrameCaptureProgress.State != RuntimeFrameCaptureState::InFlight ||
			!mFrameCaptureHandle)
		{
			return;
		}
		if (!mRuntimeRenderer->IsCaptureValid(mFrameCaptureHandle))
		{
			mFrameCaptureProgress.State = RuntimeFrameCaptureState::Failed;
			mFrameCaptureProgress.Error = "Runtime renderer invalidated the capture handle";
			mFrameCaptureHandle = {};
			return;
		}

		const TextureReadbackResult result =
			mRuntimeRenderer->GetCaptureResult(mFrameCaptureHandle);
		if (result.Status != TextureReadbackStatus::Complete &&
			result.Status != TextureReadbackStatus::Failed)
		{
			return;
		}

		mFrameCaptureProgress.State = result.Status == TextureReadbackStatus::Complete
			? RuntimeFrameCaptureState::Complete
			: RuntimeFrameCaptureState::Failed;
		mFrameCaptureProgress.Error = result.Error;
		mRuntimeRenderer->ReleaseCapture(mFrameCaptureHandle);
		mFrameCaptureHandle = {};
	}

	RuntimeProjectResourceWarmup& Application::GetProjectResourceWarmup()
	{
		if (!mProjectResourceWarmup || !mProjectResourceWarmup->IsInitialized())
			throw std::logic_error("Application project resource warm-up is not initialized.");
		return *mProjectResourceWarmup;
	}

	void Application::PushLayer(Layer* layer)
	{
		HE_PROFILE;

		mLayerStack.PushLayer(layer);
		layer->OnAttach();
	}

	void Application::PushOverlay(Layer* layer)
	{
		HE_PROFILE;

		mLayerStack.PushOverlay(layer);
		layer->OnAttach();
	}

	void Application::PopLayer(Layer* layer)
	{
		HE_PROFILE;

		mLayerStack.PopLayer(layer);
		layer->OnDetach();
	}

	void Application::Close()
	{
		HE_PROFILE;

		bRunning = false;
	}

	void Application::RequestRestart()
	{
		HE_PROFILE;

		bRestartRequested = true;
		bRunning = false;
	}

	void Application::BeginRuntimeStartup()
	{
		SimulationClock& simulationClock = SimulationClock::GetInstance();
		if (bRuntimeStartupPending)
			throw std::logic_error("Runtime startup is already pending.");
		if (simulationClock.IsFrameInProgress())
			throw std::logic_error("Runtime startup cannot begin during an active simulation frame.");

		bRuntimeStartupFirstPresented = false;
		bRuntimeStartupGameplayPresentRequested = false;
		bRuntimeStartupGameplayPresented = false;
		ClearRuntimeSceneRequest();
		mFrameCaptureHandle = {};
		mFrameCaptureProgress = {};
		bRuntimeStartupPending = true;
	}

	void Application::CompleteRuntimeStartup()
	{
		SimulationClock& simulationClock = SimulationClock::GetInstance();
		if (!bRuntimeStartupPending)
			throw std::logic_error("No runtime startup is pending.");
		if (simulationClock.IsFrameInProgress())
			throw std::logic_error("Runtime startup cannot complete during an active simulation frame.");
		if (!bRuntimeStartupFirstPresented)
			throw std::logic_error("Runtime startup cannot complete before its first presented frame.");
		if (!bRuntimeStartupGameplayPresented)
			throw std::logic_error("Runtime startup cannot complete before its first gameplay frame is presented.");

		simulationClock.Reset();
		bRuntimeStartupPending = false;
		bRuntimeStartupFirstPresented = false;
		bRuntimeStartupGameplayPresentRequested = false;
		bRuntimeStartupGameplayPresented = false;
	}

	void Application::NotifyRuntimeStartupPresentedFrame()
	{
		if (!bRuntimeStartupPending)
			throw std::logic_error("No runtime startup is pending.");
		if (bRuntimeStartupFirstPresented)
			throw std::logic_error("The runtime startup first Present was already recorded.");

		bRuntimeStartupFirstPresented = true;
	}

	void Application::RequestRuntimeStartupGameplayPresent()
	{
		SimulationClock& simulationClock = SimulationClock::GetInstance();
		if (!bRuntimeStartupPending)
			throw std::logic_error("No runtime startup is pending.");
		if (simulationClock.IsFrameInProgress())
			throw std::logic_error("A runtime startup gameplay Present cannot be requested during an active simulation frame.");
		if (!bRuntimeStartupFirstPresented)
			throw std::logic_error("A runtime startup gameplay Present cannot be requested before the first startup Present.");
		if (bRuntimeStartupGameplayPresentRequested)
			throw std::logic_error("The runtime startup gameplay Present was already requested.");

		bRuntimeStartupGameplayPresentRequested = true;
	}

	void Application::NotifyRuntimeStartupGameplayPresentedFrame()
	{
		if (!bRuntimeStartupPending)
			throw std::logic_error("No runtime startup is pending.");
		if (!bRuntimeStartupFirstPresented)
			throw std::logic_error("A runtime startup gameplay Present cannot be recorded before the first startup Present.");
		if (!bRuntimeStartupGameplayPresentRequested)
			throw std::logic_error("No runtime startup gameplay Present was requested.");
		if (bRuntimeStartupGameplayPresented)
			throw std::logic_error("The runtime startup gameplay Present was already recorded.");

		bRuntimeStartupGameplayPresented = true;
	}

	void Application::CancelRuntimeStartup() noexcept
	{
		bRuntimeStartupPending = false;
		bRuntimeStartupFirstPresented = false;
		bRuntimeStartupGameplayPresentRequested = false;
		bRuntimeStartupGameplayPresented = false;
	}

	std::uint64_t Application::GetFrameNumber() const
	{
		return SimulationClock::GetInstance().GetFrameNumber();
	}

	std::uint64_t Application::GetCompletedFrameCount() const
	{
		return SimulationClock::GetInstance().GetCompletedFrameCount();
	}

	double Application::GetSimulationTimeSeconds() const
	{
		return SimulationClock::GetInstance().GetElapsedSeconds();
	}

	void Application::OnEvent(Event& e)
	{
		HE_PROFILE;

		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<WindowCloseEvent>(HE_BIND_EVENT_FN(Application::OnWindowClose));
		dispatcher.Dispatch<WindowResizeEvent>(HE_BIND_EVENT_FN(Application::OnWindowResize));

#if HE_ENABLE_GM
		if (!e.Handled)
			(void)GMPanelService::GetInstance().OnEvent(e);
#endif

		for (auto it = mLayerStack.rbegin(); it != mLayerStack.rend(); ++it)
		{
			if (e.Handled)
				break;

			HE_PROFILE_TRANSIENT_SECTION((*it)->GetName().c_str());
			(*it)->OnEvent(e);
		}
	}

	void Application::Init(const std::string& name)
	{
		HE_PROFILE;
		bRunning = true;
		bMinimized = false;
		bCleaned = false;
		bRestartRequested = false;
		bRuntimeStartupPending = false;
		bRuntimeStartupFirstPresented = false;
		bRuntimeStartupGameplayPresentRequested = false;
		bRuntimeStartupGameplayPresented = false;
		ClearRuntimeSceneRequest();
		SimulationClock::GetInstance().Reset();
		PresentationClock::GetInstance().Reset();
		HE_PROFILE_THREAD("Main Thread");
		HE_PROFILE_APP_INFO(name.c_str(), name.size());

		Log::Init();
		HE_CORE_INFO("[Engine] HEngine {0}", EngineVersion::String.data());
		if (!AudioEngine::GetInstance().Initialize())
			HE_CORE_WARN("[Audio] Audio output is unavailable; continuing without sound");

#ifdef WITH_EDITOR
		ModuleManager::Init();
#endif
		// Static plugin anchors are installed by the host composition root.
		// PreDevice plugins (for example capture tooling) start before Window/RHI.
		ModuleManager::GetInstance().StartPluginsThroughPhase(
			PluginLoadPhase::PreDevice);

		Reflection::TypeMetaRegister::metaRegister();

		RenderSettingsService& renderSettingsService =
			RenderSettingsService::GetInstance();
		Json commandLineRenderOverrides = Json::object();
		if (mRunOptions.GraphicsApiOverride)
		{
			commandLineRenderOverrides[
				std::string(RenderSettingIds::Backend)] =
				ToString(*mRunOptions.GraphicsApiOverride);
		}
		else if (mRunOptions.bUseWarp)
		{
			// WARP is an explicit DX12 adapter request and therefore also wins
			// over a persisted Vulkan preference.
			commandLineRenderOverrides[
				std::string(RenderSettingIds::Backend)] = "DX12";
		}
		if (mRunOptions.bNoVSync)
		{
			commandLineRenderOverrides[
				std::string(RenderSettingIds::VSync)] = false;
		}
		if (mRunOptions.bRenderValidation)
		{
			commandLineRenderOverrides[
				std::string(RenderSettingIds::RenderGraphValidation)] = true;
		}
		const RenderDebugSettings environmentDebug =
			ReadRenderDebugSettingsFromEnvironment();
		if (environmentDebug.PbrDebugMode != 0)
		{
			commandLineRenderOverrides[
				std::string(RenderSettingIds::PbrDebugMode)] =
				environmentDebug.PbrDebugMode;
		}
		if (environmentDebug.bShadowCascadeDebug)
		{
			commandLineRenderOverrides[
				std::string(RenderSettingIds::ShadowCascadeDebug)] = true;
		}
		if (!commandLineRenderOverrides.empty())
			renderSettingsService.SetCommandLineOverrides(
				commandLineRenderOverrides);
		const ResolvedRenderSettings initialRenderSettings =
			renderSettingsService.GetSnapshot();

		WindowProps windowProps(name);
		windowProps.bVSync = initialRenderSettings.Effective.bVSync;
#if !defined(WITH_EDITOR)
		// Gameplay has no text fields. Disable the Windows IME before GLFW creates
		// the first top-level window so TSF cannot turn movement keys into VK_PROCESSKEY.
		windowProps.bEnableInputMethod = false;
#endif
		if (mRunOptions.HasFixedWindowSize())
		{
			windowProps.Width = mRunOptions.WindowWidth;
			windowProps.Height = mRunOptions.WindowHeight;
			windowProps.bMaximized = false;
			windowProps.bResizable = false;
			windowProps.bAllowFullscreen = false;
		}
		mWindow = Window::Create(windowProps);
		mWindow->SetEventCallback(HE_BIND_EVENT_FN(Application::OnEvent));

		auto* renderSurface = dynamic_cast<RenderSurface*>(mWindow.get());
		if (!renderSurface)
			throw std::runtime_error("The active Window does not provide a RenderSurface");

		const std::filesystem::path shaderCache =
			ConfigManager::GetEngineGeneratedContentFullPath("Shaders/CACHES/DXIL");
		const AntiAliasingSettings antiAliasing =
			renderSettingsService.BuildRequestedRuntimeAntiAliasing();
		const GlobalIlluminationSettings globalIllumination =
			renderSettingsService.BuildRequestedRuntimeGlobalIllumination();
		RuntimeRendererDesc rendererDesc{
			.Surface = renderSurface,
			.Backend = initialRenderSettings.Effective.Backend,
			.AdapterSelection = {
				.bUseWarp = mRunOptions.bUseWarp,
			},
			.ShaderCacheDirectory = shaderCache,
			.AssetRootDirectory = ConfigManager::GetInstance().GetRootFolder(),
			.EngineAssetRootDirectory = ConfigManager::GetInstance().GetEngineContentFolder(),
			// Validation is intentionally opt-in. The DX12 debug layer and NVRHI
			// validation are indispensable diagnostics, but are not a representative
			// editor-performance configuration.
			.bEnableValidation =
				initialRenderSettings.Debug.bRenderGraphValidation,
			.bEnableGpuTiming = mRunOptions.bRenderStats,
			.GlobalIllumination = globalIllumination,
			.AntiAliasing = antiAliasing,
			.PbrDebugMode = initialRenderSettings.Debug.PbrDebugMode,
			.bShadowCascadeDebug =
				initialRenderSettings.Debug.bShadowCascadeDebug,
		};
		const RenderBackend requestedBackend = rendererDesc.Backend;
		std::string backendFallbackReason;
		const auto initializeRenderer = [this, &rendererDesc]()
		{
			mRuntimeRenderer = CreateScope<RuntimeRenderer>();
			mRuntimeRenderer->Initialize(rendererDesc);
		};
		try
		{
			initializeRenderer();
		}
		catch (const std::exception& exception)
		{
			if (requestedBackend != RenderBackend::Vulkan ||
				mRunOptions.bStrictGraphicsApi ||
				!IsRenderBackendCompiled(RenderBackend::DX12))
			{
				std::throw_with_nested(std::runtime_error(
					std::string("Failed to initialize requested ") +
					ToString(requestedBackend) + " renderer"));
			}

			backendFallbackReason = std::string(
				"Vulkan initialization failed: ") + exception.what();
			HE_CORE_ERROR("[Graphics] {0}", backendFallbackReason);
			HE_CORE_WARN(
				"[Graphics] Falling back once to DX12. Use --strict-graphics-api to turn this fallback into a startup failure.");
			mRuntimeRenderer.reset();
			rendererDesc.Backend = RenderBackend::DX12;
			rendererDesc.AdapterSelection.bUseWarp = mRunOptions.bUseWarp;
			initializeRenderer();
		}
		catch (...)
		{
			if (requestedBackend != RenderBackend::Vulkan ||
				mRunOptions.bStrictGraphicsApi ||
				!IsRenderBackendCompiled(RenderBackend::DX12))
			{
				throw;
			}
			backendFallbackReason =
				"Vulkan initialization failed with a non-standard exception.";
			HE_CORE_ERROR("[Graphics] {0}", backendFallbackReason);
			HE_CORE_WARN("[Graphics] Falling back once to DX12.");
			mRuntimeRenderer.reset();
			rendererDesc.Backend = RenderBackend::DX12;
			rendererDesc.AdapterSelection.bUseWarp = mRunOptions.bUseWarp;
			initializeRenderer();
		}
		HE_CORE_INFO("[Graphics] Requested backend: {0}",
			ToString(requestedBackend));
		HE_CORE_INFO("[Graphics] Active backend: {0}",
			ToString(mRuntimeRenderer->GetBackend()));
		ModuleManager::GetInstance().StartPluginsThroughPhase(
			PluginLoadPhase::PostDevice);

		RenderSettingsCapabilities settingsCapabilities;
		settingsCapabilities.Render = mRuntimeRenderer->GetCapabilities();
		for (const RenderBackend backend : {
				RenderBackend::DX12,
				RenderBackend::Vulkan })
		{
			settingsCapabilities.BackendAvailability[backend] = {
				IsRenderBackendCompiled(backend),
				GetRenderBackendAvailabilityReason(backend),
			};
		}
		if (!backendFallbackReason.empty())
		{
			settingsCapabilities.BackendAvailability[RenderBackend::Vulkan] = {
				false,
				backendFallbackReason,
			};
		}
		settingsCapabilities.AntiAliasing =
			mRuntimeRenderer->GetAntiAliasingCapabilities();
		settingsCapabilities.bScreenSpaceGiAvailable = true;
		settingsCapabilities.bExternalViewportRendererAvailable =
			!mRuntimeRenderer->EnumerateOfflineViewportRenderers().empty();
		renderSettingsService.SetCapabilities(std::move(settingsCapabilities));
		renderSettingsService.SetRuntimeApplyCallback(
			[this](const ResolvedRenderSettings& settings,
				const RenderSettingsChangePlan& plan)
			{
				if (!mWindow || !mRuntimeRenderer)
					throw std::logic_error(
						"Runtime render settings require an initialized Application");
				mWindow->SetVSync(settings.Effective.bVSync);
				ConfigManager::GetInstance().VSync =
					settings.Effective.bVSync;
				mRuntimeRenderer->ApplyRenderSettingsBatch(settings, plan);
			});

		mImGuiLayer = ImGuiLayer::CreateNvrhi(NvrhiImGuiLayerDesc{
			.MainWindow = mWindow.get(),
			.Renderer = NvrhiImGuiRendererDesc{
				.Device = mRuntimeRenderer->GetDevice(),
				.FramesInFlight = mRuntimeRenderer->GetFramesInFlight(),
				.VertexShaderArtifact = shaderCache / "NvrhiImGui.vs",
				.PixelShaderArtifact = shaderCache / "NvrhiImGui.ps",
				.DebugName = "HEngine Runtime ImGui",
			},
			.bInstallGlfwCallbacks = true,
		});
		mNvrhiImGuiLayer = dynamic_cast<NvrhiImGuiLayer*>(mImGuiLayer);
		if (!mNvrhiImGuiLayer)
			throw std::runtime_error("NVRHI ImGui factory returned an incompatible layer");
		mUiTextureRegistry = CreateScope<RuntimeUiTextureRegistry>();
		PushOverlay(mImGuiLayer);

		RuntimeResourceRegistry& resourceRegistry = RuntimeResourceRegistry::GetInstance();
		resourceRegistry.Init();
		mProjectResourceWarmup = CreateScope<RuntimeProjectResourceWarmup>();
		mProjectResourceWarmup->Initialize(
			resourceRegistry,
			ConfigManager::GetInstance().GetRootFolder(),
			ConfigManager::GetInstance().GetEngineContentFolder(),
			ConfigManager::GetInstance().GetGameConfigPath(),
			RuntimeResourceCatalog::GetInstance().GetSnapshot());
		RmlUiManager::GetInstance().InitNvrhi(
			NvrhiRmlRenderBackendDesc{
				.Device = mRuntimeRenderer->GetDevice(),
				.FramesInFlight = mRuntimeRenderer->GetFramesInFlight(),
				.VertexShaderArtifact = shaderCache / "NvrhiRml.vs",
				.PixelShaderArtifact = shaderCache / "NvrhiRml.ps",
				.DebugName = "HEngine Runtime RmlUi",
			},
			*mWindow);
#if HE_ENABLE_GM
		GMPanelService::GetInstance().Initialize();
#endif
		ModuleManager::GetInstance().StartPluginsThroughPhase(
			PluginLoadPhase::PostEngine);
	}

	RenderBackend Application::GetRenderBackend() const
	{
		if (!mRuntimeRenderer)
			throw std::logic_error("The runtime renderer is not initialized");
		return mRuntimeRenderer->GetBackend();
	}

	void Application::Run()
	{
		auto& simulationClock = SimulationClock::GetInstance();
		auto& presentationClock = PresentationClock::GetInstance();
		bool bCaptureCompleted = false;
		TextureReadbackHandle automationCapture;
		constexpr std::uint64_t c_PerformanceWarmupFrames = 30;
		std::uint64_t measuredFrameCount = 0;
		double measuredFrameSeconds = 0.0;

		while (bRunning)
		{
			const auto measuredFrameStart = std::chrono::steady_clock::now();
			HE_PROFILE_FRAME("HEngine Main Frame");
			HE_PROFILE_SECTION("Application::Frame");

			RuntimeResourceRegistry& resourceRegistry = RuntimeResourceRegistry::GetInstance();
			if (resourceRegistry.IsInitialized())
			{
				(void)resourceRegistry.PumpMainThreadWork(std::chrono::milliseconds(2), 1);
				if (mProjectResourceWarmup && mProjectResourceWarmup->IsInitialized())
					mProjectResourceWarmup->Pump();
#if defined(WITH_EDITOR)
				if ((simulationClock.GetFrameNumber() % 120u) == 0u)
					(void)resourceRegistry.CollectUnused();
#else
				(void)resourceRegistry.CollectUnused();
#endif
			}

			{
				HE_PROFILE_SECTION("Layer Pre-Updates");
				for (Layer* layer : mLayerStack)
					layer->OnPreUpdate();
			}
			if (!bRunning)
				break;

			const bool bAutomationFrame = !bRuntimeStartupPending;
			const Timestep presentationTimestep = presentationClock.BeginFrame(
				mRunOptions.FixedDeltaSeconds,
				bAutomationFrame && !bMinimized);
			const Timestep timestep = simulationClock.BeginFrame(
				mRunOptions.FixedDeltaSeconds,
				bAutomationFrame);
			AudioEngine::GetInstance().Update();
			HE_PROFILE_PLOT("Frame Time (ms)", timestep.GetMilliseconds());
			HE_PROFILE_PLOT("Presentation Frame Time (ms)", presentationTimestep.GetMilliseconds());
			HE_PROFILE_PLOT("FPS", timestep.GetSeconds() > 0.0f ? 1.0f / timestep.GetSeconds() : 0.0f);

			const bool bFrameRendered = !bMinimized;
			if (bFrameRendered)
			{
				{
					HE_PROFILE_SECTION("Layer Updates");
					for (Layer* layer : mLayerStack)
					{
						HE_PROFILE_TRANSIENT_SECTION(layer->GetName().c_str());
						layer->OnUpdate(timestep);
					}
				}

				const bool bCaptureThisFrame = bAutomationFrame &&
					mRunOptions.HasFrameCapture() &&
					simulationClock.GetFrameNumber() == mRunOptions.CaptureFrame;
				const bool bRuntimeCaptureThisFrame =
					mFrameCaptureProgress.State == RuntimeFrameCaptureState::Queued;
				if (bCaptureThisFrame && bRuntimeCaptureThisFrame)
					throw std::runtime_error("Automation and runtime captures requested the same frame");
				RuntimeUiOverlayRecorder overlayRecorder(
					*mNvrhiImGuiLayer,
					RmlUiManager::GetInstance());
				RuntimeRendererFrameResult frameResult;
				mNvrhiImGuiLayer->BeginCpuFrame();
				try
				{
					{
						HE_PROFILE_SECTION("ImGui Frame");
						for (Layer* layer : mLayerStack)
						{
							HE_PROFILE_TRANSIENT_SECTION(layer->GetName().c_str());
							layer->OnImGuiRender();
						}
					}

					frameResult = mRuntimeRenderer->RenderFrame(
						RuntimeRendererFrameDesc{
							.World = mRenderWorld,
							.OverlayRecorder = &overlayRecorder,
							.CapturePath = bCaptureThisFrame
								? mRunOptions.CapturePath
								: (bRuntimeCaptureThisFrame
									? mFrameCaptureProgress.OutputPath
									: std::filesystem::path{}),
						},
						mWindow->IsVSync());
				}
				catch (...)
				{
					mNvrhiImGuiLayer->CancelCpuFrame();
					throw;
				}

				if (bCaptureThisFrame)
				{
					if (!frameResult.Capture)
						throw std::runtime_error("The RenderGraph capture pass did not queue a readback");
					automationCapture = frameResult.Capture;
				}
				else if (bRuntimeCaptureThisFrame)
				{
					if (!frameResult.Capture)
						throw std::runtime_error("The runtime capture pass did not queue a readback");
					mFrameCaptureHandle = frameResult.Capture;
					mFrameCaptureProgress.State = RuntimeFrameCaptureState::InFlight;
				}
				mLastFrameSlot = frameResult.FrameSlot;
				mLastSubmission = frameResult.Submission;

				if (bRuntimeStartupPending && !bRuntimeStartupFirstPresented)
				{
					HE_PROFILE_SECTION("Application::RuntimeStartupFirstPresent");
					NotifyRuntimeStartupPresentedFrame();
				}
				else if (bRuntimeStartupPending &&
					bRuntimeStartupGameplayPresentRequested &&
					!bRuntimeStartupGameplayPresented && bRunning)
				{
					HE_PROFILE_SECTION("Application::RuntimeStartupFirstGameplayPresent");
					NotifyRuntimeStartupGameplayPresentedFrame();
				}
			}
			else if (bAutomationFrame && mRunOptions.HasFrameCapture() &&
				simulationClock.GetFrameNumber() == mRunOptions.CaptureFrame)
			{
				throw std::runtime_error("Cannot capture an automation frame while the window is minimized.");
			}

			{
				HE_PROFILE_SECTION("Window Update");
				mWindow->OnUpdate();
			}

			simulationClock.CompleteFrame();
			presentationClock.CompleteFrame();
			if (mRunOptions.bRenderStats &&
				simulationClock.GetCompletedFrameCount() > c_PerformanceWarmupFrames)
			{
				measuredFrameSeconds += std::chrono::duration<double>(
					std::chrono::steady_clock::now() - measuredFrameStart).count();
				++measuredFrameCount;
			}
			PollFrameCapture();
			if (automationCapture && mRuntimeRenderer->IsCaptureValid(automationCapture))
			{
				const TextureReadbackResult result =
					mRuntimeRenderer->GetCaptureResult(automationCapture);
				if (result.Status == TextureReadbackStatus::Complete)
				{
					bCaptureCompleted = true;
					HE_CORE_INFO(
						"[Automation] Captured frame {0}: {1} (target=presented-backbuffer)",
						mRunOptions.CaptureFrame,
						Utils::PathToUtf8(result.OutputPath));
					mRuntimeRenderer->ReleaseCapture(automationCapture);
					automationCapture = {};
				}
				else if (result.Status == TextureReadbackStatus::Failed)
				{
					throw std::runtime_error("Automation capture failed: " + result.Error);
				}
			}

			const std::uint64_t completedFrameCount = simulationClock.GetCompletedFrameCount();
			if (bAutomationFrame && mRunOptions.ExitAfterFrames > 0 &&
				completedFrameCount >= mRunOptions.ExitAfterFrames)
			{
				HE_CORE_INFO("[Automation] Completed frame limit: {0}", completedFrameCount);
				bRunning = false;
			}
		}

		if (automationCapture)
		{
			mRuntimeRenderer->WaitForIdle();
			const TextureReadbackResult result =
				mRuntimeRenderer->GetCaptureResult(automationCapture);
			if (result.Status != TextureReadbackStatus::Complete)
				throw std::runtime_error("Automation capture did not complete: " + result.Error);
			bCaptureCompleted = true;
			HE_CORE_INFO(
				"[Automation] Captured frame {0}: {1} (target=presented-backbuffer)",
				mRunOptions.CaptureFrame,
				Utils::PathToUtf8(result.OutputPath));
			mRuntimeRenderer->ReleaseCapture(automationCapture);
		}
		if (mFrameCaptureProgress.State == RuntimeFrameCaptureState::InFlight)
		{
			mRuntimeRenderer->WaitForIdle();
			PollFrameCapture();
		}

		if (mRunOptions.HasFrameCapture() && !bCaptureCompleted)
			throw std::runtime_error("The application closed before the requested automation frame was captured.");
		if (mRunOptions.bRenderStats)
		{
			// GPU timestamps are resolved only after their owning submissions have
			// retired. This is the one intentional synchronization point for stats.
			mRuntimeRenderer->WaitForIdle();
			const double averageMilliseconds = measuredFrameCount > 0
				? measuredFrameSeconds * 1000.0 / static_cast<double>(measuredFrameCount)
				: 0.0;
			const double framesPerSecond = measuredFrameSeconds > 0.0
				? static_cast<double>(measuredFrameCount) / measuredFrameSeconds
				: 0.0;
			const std::string sceneStats = mRuntimeRenderer->BuildSceneStatsSummary();
			std::fprintf(
				stdout,
				"[RenderPerformance] samples=%llu avgMs=%.3f cpuAvgMs=%.3f fps=%.1f validation=%u %s\n",
				static_cast<unsigned long long>(measuredFrameCount),
				averageMilliseconds,
				averageMilliseconds,
				framesPerSecond,
				mRunOptions.bRenderValidation ? 1u : 0u,
				sceneStats.c_str());
			const std::string graphDump = mRuntimeRenderer->BuildRenderGraphDebugDump();
			std::fprintf(stdout, "[RenderGraphDump]\n%s\n", graphDump.c_str());
			std::fflush(stdout);
		}
	}

	void Application::Clean()
	{
		HE_PROFILE;
		if (bCleaned)
			return;
		bRunning = false;
		CancelRuntimeStartup();
		ClearRuntimeSceneRequest();

		// Retire all submitted GPU work before UI layers release their NVRHI handles.
		if (mRuntimeRenderer)
		{
			mRuntimeRenderer->WaitForIdle();
			PollFrameCapture();
		}
		// Rml owns global interfaces. Tear its context down while the injected
		// RenderInterface and RenderDevice are both alive; layer detachment must
		// never be able to strand those globals during exception cleanup.
#if HE_ENABLE_GM
		GMPanelService::GetInstance().Shutdown();
#endif
		RmlUiManager::GetInstance().Shutdown();
		if (mUiTextureRegistry)
		{
			std::vector<ImGuiTextureHandle> registrations;
			registrations.reserve(mUiTextureRegistry->Registrations.size());
			for (const auto& [key, managed] : mUiTextureRegistry->Registrations)
			{
				registrations.push_back(ImGuiTextureHandle{
					.Index = static_cast<std::uint32_t>(key),
					.Generation = static_cast<std::uint32_t>(key >> 32u),
				});
			}
			for (const ImGuiTextureHandle handle : registrations)
				ReleaseImGuiTexture(handle);
			registrations.clear();
			registrations.reserve(mUiTextureRegistry->ExternalRegistrations.size());
			for (const auto& [key, handle] : mUiTextureRegistry->ExternalRegistrations)
				registrations.push_back(handle);
			for (const ImGuiTextureHandle handle : registrations)
				UnregisterImGuiTexture(handle);
		}
		mLayerStack.Shutdown();
		mImGuiLayer = nullptr;
		mNvrhiImGuiLayer = nullptr;
		mUiTextureRegistry.reset();
		mRenderWorld.reset();
		mFrameCaptureHandle = {};
		mFrameCaptureProgress = {};
		if (mProjectResourceWarmup)
		{
			mProjectResourceWarmup->Shutdown();
			mProjectResourceWarmup.reset();
		}
		// Consumers and UI callbacks are detached, GPU work is idle, and the
		// render device/resource services are still valid. Plugins must stop in
		// this window so GPU-owning modules can cancel, retire, and release their
		// resources explicitly instead of relying on device destruction.
		try
		{
			ModuleManager::GetInstance().StopPlugins();
		}
		catch (const std::exception& exception)
		{
			Log::WriteFallbackError(
				"Static plugin shutdown reported a failure.",
				exception.what());
		}

		RuntimeResourceRegistry& resourceRegistry = RuntimeResourceRegistry::GetInstance();
		if (resourceRegistry.IsInitialized())
		{
			(void)resourceRegistry.CollectUnused();
			resourceRegistry.Shutdown();
		}

		if (mRuntimeRenderer)
		{
			RenderSettingsService::GetInstance().SetRuntimeApplyCallback({});
			mRuntimeRenderer->Shutdown();
			mRuntimeRenderer.reset();
		}
		AudioEngine::GetInstance().Shutdown();
		mWindow.reset();
		bCleaned = true;
	}

	bool Application::OnWindowClose(WindowCloseEvent& e)
	{
		HE_PROFILE;

		bRunning = false;
		return true;
	}

	bool Application::OnWindowResize(WindowResizeEvent& e)
	{
		HE_PROFILE;

		if (e.GetWidth() == 0 || e.GetHeight() == 0)
		{
			bMinimized = true;
			return false;
		}

		bMinimized = false;
		if (mRuntimeRenderer)
			mRuntimeRenderer->Resize(e.GetWidth(), e.GetHeight());
		ConfigManager::mViewportSize = {
			static_cast<float>(e.GetWidth()),
			static_cast<float>(e.GetHeight())
		};

		return false;
	}
}
