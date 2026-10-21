#include "Hepch.h"
#include "Application.h"

#include "Runtime/Audio/AudioEngine.h"
#include "Runtime/Core/Input/Input.h"
#include "Runtime/Core/Input/InputActions.h"
#include "Runtime/Scripting/ScriptInputContext.h"
#include "Runtime/Platform/Scripting/ScriptPlatformServices.h"
#include "Runtime/Core/PresentationClock.h"
#include "Runtime/Core/SimulationClock.h"
#include "Runtime/Core/RuntimeCpuBudget.h"
#include "Runtime/Graphics/Material/MaterialShaderManager.h"
#include "Runtime/Graphics/Material/Material.h"
#include "Runtime/Graphics/Material/MaterialResourceCache.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIlluminationSettingsJson.h"
#include "Runtime/Graphics/RHI/RenderSurface.h"
#include "Runtime/Graphics/RHI/ShaderArtifactResolver.h"
#include "Runtime/Graphics/Renderer/AntiAliasing.h"
#include "Runtime/Graphics/Renderer/RuntimeRenderer.h"
#include "Runtime/Graphics/Renderer/SceneRenderer.h"
#include "Runtime/Graphics/Settings/RenderSettingsService.h"
#include "Runtime/ImGui/NvrhiImGuiLayer.h"
#include "Runtime/Profile/RenderFrameTiming.h"
#include "Runtime/Profile/CpuExecutionTimes.h"
#include "Runtime/Profile/BoundedStatisticsBuffer.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/ConfigManager/GameDisplaySettings.h"
#include "Runtime/Resource/RuntimeProjectResourceWarmup.h"
#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceCatalog.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeResourceRegistry.h"
#include "Runtime/Core/Meta/Reflection/ReflectionRegister.h"
#include "Runtime/Core/Modules/ModuleManager.h"
#include "Runtime/UI/Rml/RmlUiManager.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/FileUtils/AtomicFile.h"

#if HE_ENABLE_GM
#include "Runtime/DevTools/GM/GMPanelService.h"
#endif

#include <HEngine/EngineVersion.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace HEngine
{
	struct RuntimeUiTextureRegistry
	{
		struct Registration
		{
			ManagedTextureHandle Managed;
			nvrhi::TextureHandle BoundTexture;
		};
		std::unordered_map<std::uint64_t, Registration> Registrations;
		std::unordered_map<std::uint64_t, ImGuiTextureHandle> ExternalRegistrations;
	};

	namespace
	{
		void WriteRenderingSettingsEvidence(const ApplicationRunOptions& options,
			const RuntimeRenderer& renderer, const Window& window, std::string_view phase)
		{
			if (options.RenderStatsPath.empty()) return;
			const auto snapshot = RenderSettingsService::GetInstance().GetSnapshot();
			Json evidence = SerializeResolvedRenderSettings(snapshot, false);
			evidence["serviceEffective"] = evidence["effective"];
            evidence["performanceMeasurement"] = {
                { "version", 2 },
                { "metric", "cpuWallFrameMs" },
                { "start", "after Window.WaitForFrame and resume handling" },
                { "end", "after statistics snapshot, CPU graph bookkeeping and timing guard cancellation" },
                { "includesStatisticsBookkeeping", true },
                { "includesTimingGuardCancellation", true },
                { "legacyWithoutVersion", "v1: end before statistics snapshot and CPU graph bookkeeping" },
                { "statisticsOutput", "bounded structured buffers; CSV formatting and I/O after measured loop" },
                { "statisticsCaptureCostIncluded", true },
            };
			RenderSettings actual = snapshot.Effective;
			actual.Backend = renderer.GetBackend();
			actual.bVSync = window.IsVSync();
			actual.AntiAliasing = renderer.GetEffectiveAntiAliasingSettings();
			actual.GlobalIllumination = renderer.GetEffectiveGlobalIlluminationSettings();
			evidence["effective"] = RenderSettingsToFlatValues(actual);
			const auto aa = renderer.GetAntiAliasingSelection();
			const auto gi = renderer.GetGlobalIlluminationSelection();
			const auto sceneOutput = renderer.GetMainOffscreenOutput();
			evidence["renderer"] = {
				{ "phase", phase }, { "width", renderer.GetWidth() }, { "height", renderer.GetHeight() },
                { "sceneWidth", sceneOutput ? sceneOutput.Width : renderer.GetWidth() },
                { "sceneHeight", sceneOutput ? sceneOutput.Height : renderer.GetHeight() },
                { "sceneOffscreen", static_cast<bool>(sceneOutput) },
                { "gpuTimingScope", sceneOutput ? "presentation graph; offscreen scene timing is separate" : "main scene graph" },
                { "framesInFlight", renderer.GetFramesInFlight() },
				{ "antiAliasingFallback", aa.bFallback || aa.bBypassed },
				{ "antiAliasingReason", aa.Reason }, { "globalIlluminationFallback", gi.bFallback },
				{ "globalIlluminationReason", gi.Reason },
				{ "settingsInputPath", Utils::PathToUtf8(options.RenderSettingsPath) },
			};
			auto outputPath = options.RenderStatsPath;
			outputPath += ".settings.json";
			std::filesystem::create_directories(outputPath.parent_path());
			std::ofstream output(outputPath);
			if (!output) throw std::runtime_error("Could not create rendering settings evidence");
			output << evidence.dump(2) << '\n';
			output.flush();
			if (!output) throw std::runtime_error("Could not write rendering settings evidence");
		}

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
                return mRmlUi.HasActiveDocument() && !mRmlUi.IsFrameRenderingSuppressed();
            }

            [[nodiscard]] RuntimeUiRenderMode GetSceneOverlayMode() const noexcept override
            {
                return mRmlUi.HasActiveDocument() && !mRmlUi.IsFrameRenderingSuppressed()
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
						static const bool bTimeRmlRecord = [] {
							const char* setting = std::getenv("HE_RML_RECORD_TIMINGS");
							return setting && setting[0] == '1' && setting[1] == '\0';
						}();
						const auto recordStart = bTimeRmlRecord
							? std::chrono::steady_clock::now()
							: std::chrono::steady_clock::time_point{};
						const auto beforeStats = bTimeRmlRecord
							? mRmlUi.GetRenderStats()
							: NvrhiRmlRenderBackendStats{};
						mRmlUi.BeginGpuFrame(
							context.CommandList,
							context.Framebuffer,
							context.FrameSlot,
							context.CompletedSubmission,
                            context.BackdropTexture);
						const auto beginEnd = bTimeRmlRecord
							? std::chrono::steady_clock::now()
							: std::chrono::steady_clock::time_point{};
						mRmlUi.Render();
						const auto renderEnd = bTimeRmlRecord
							? std::chrono::steady_clock::now()
							: std::chrono::steady_clock::time_point{};
						mRmlUi.EndGpuFrame();
						if (bTimeRmlRecord)
						{
							const auto recordEnd = std::chrono::steady_clock::now();
							const double totalMs = std::chrono::duration<double, std::milli>(
								recordEnd - recordStart).count();
							if (totalMs > 8.0)
							{
								const auto afterStats = mRmlUi.GetRenderStats();
								HE_CORE_INFO(
									"[RmlRecordTiming] slot={} beginMs={:.3f} renderMs={:.3f} endMs={:.3f} totalMs={:.3f} draws={} geometryUploads={} geometryBuffersReused={} textureUploads={} filters={} layerComposites={} customDraws={} liveGeometry={} liveTextures={} geometryCompileMs={:.3f} bufferCreateMs={:.3f} bufferWriteMs={:.3f} geometryDrawMs={:.3f} buffersCreated={} createdBytes={} createdLe512={} createdLe4096={} arenaAllocations={}",
									context.FrameSlot,
									std::chrono::duration<double, std::milli>(beginEnd - recordStart).count(),
									std::chrono::duration<double, std::milli>(renderEnd - beginEnd).count(),
									std::chrono::duration<double, std::milli>(recordEnd - renderEnd).count(),
									totalMs,
									afterStats.DrawCalls - beforeStats.DrawCalls,
									afterStats.GeometryUploads - beforeStats.GeometryUploads,
									afterStats.GeometryBuffersReused - beforeStats.GeometryBuffersReused,
									afterStats.TextureUploads - beforeStats.TextureUploads,
									afterStats.FilterPasses - beforeStats.FilterPasses,
									afterStats.LayerComposites - beforeStats.LayerComposites,
									afterStats.CustomShaderDraws - beforeStats.CustomShaderDraws,
									afterStats.LiveGeometries,
									afterStats.LiveTextures,
									(afterStats.GeometryCompileNanoseconds - beforeStats.GeometryCompileNanoseconds) / 1'000'000.0,
									(afterStats.GeometryBufferCreateNanoseconds - beforeStats.GeometryBufferCreateNanoseconds) / 1'000'000.0,
									(afterStats.GeometryBufferWriteNanoseconds - beforeStats.GeometryBufferWriteNanoseconds) / 1'000'000.0,
									(afterStats.GeometryDrawNanoseconds - beforeStats.GeometryDrawNanoseconds) / 1'000'000.0,
									afterStats.GeometryBuffersCreated - beforeStats.GeometryBuffersCreated,
									afterStats.GeometryBufferBytesCreated - beforeStats.GeometryBufferBytesCreated,
									afterStats.GeometryBuffersCreatedLe512 - beforeStats.GeometryBuffersCreatedLe512,
									afterStats.GeometryBuffersCreatedLe4096 - beforeStats.GeometryBuffersCreatedLe4096,
									afterStats.GeometryArenaAllocations - beforeStats.GeometryArenaAllocations);
							}
						}
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

	bool Application::RequestRuntimeScene(std::string scenePath, bool bForceReload)
	{
		if (scenePath.empty() || mRuntimeSceneRequest)
			return false;
		mRuntimeSceneRequest = std::move(scenePath);
		bForceRuntimeSceneReload = bForceReload;
		return true;
	}

	std::optional<std::string> Application::ConsumeRuntimeSceneRequest(bool* forceReload)
	{
		if (forceReload) *forceReload = mRuntimeSceneRequest.has_value() && bForceRuntimeSceneReload;
		std::optional<std::string> request = std::move(mRuntimeSceneRequest);
		mRuntimeSceneRequest.reset();
		bForceRuntimeSceneReload = false;
		return request;
	}

	void Application::ClearRuntimeSceneRequest() noexcept
	{
		mRuntimeSceneRequest.reset();
		bForceRuntimeSceneReload = false;
	}

	void Application::SubmitRenderWorld(Ref<const RenderWorld> world)
	{
		mRenderWorld = std::move(world);
	}

	Ref<const RenderWorld> Application::GetLastSubmittedRenderWorld() const noexcept
	{
		return mRuntimeRenderer ? mRuntimeRenderer->GetLastSubmittedRenderWorld() : Ref<const RenderWorld>{};
	}

	MaterialShaderRefreshResult Application::RefreshMaterialShaderAtFrameBoundary(
		std::string_view sourcePath)
	{
		if (!mRuntimeRenderer)
			throw std::logic_error(
				"Application material shader refresh requires RuntimeRenderer");
        auto result = mRuntimeRenderer->RefreshMaterialShaderAtFrameBoundary(sourcePath);
        if (result.Succeeded() && result.Program)
        {
            // Publish CPU layouts before the next immutable extraction. Merely
            // replacing the GPU program leaves snapshots pinned to the old ABI
            // and generation, including those consumed by independent views.
            for (const auto& material : MaterialResourceCache::GetInstance().GetLiveMaterialsForShader(sourcePath))
            {
                if (!material || !material->IsCustomSurfaceShader()) continue;
                const auto refresh = material->RefreshShaderLayoutFromPublishedProgram(
                    result.Program->GetManifest(), result.Program->GetIdentity(), result.Program->GetGeneration());
                if (!refresh.bSucceeded)
                    result.Diagnostics.push_back({MaterialShaderDiagnosticSeverity::Error,
                        "live_material_layout", refresh.Error, material->mMatResPath});
            }
        }
        return result;
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
			.bRequireReady = false,
			.DebugName = "ImGui Asset " + Utils::PathToUtf8(path.filename()),
		});
		ImGuiTextureHandle imgui;
		try
		{
			const ManagedTexture& texture = mRuntimeRenderer->GetTexture(managed);
			imgui = mNvrhiImGuiLayer->RegisterTexture(
				texture.Texture,
				nullptr,
				nvrhi::Format::UNKNOWN);
			mUiTextureRegistry->Registrations.emplace(MakeImGuiTextureKey(imgui),
				RuntimeUiTextureRegistry::Registration{
					.Managed = managed,
					.BoundTexture = texture.Texture,
				});
			return imgui;
		}
		catch (...)
		{
			if (imgui && mNvrhiImGuiLayer->IsTextureValid(imgui))
				mNvrhiImGuiLayer->UnregisterTexture(imgui, mLastFrameSlot);
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
			mRuntimeRenderer->ReleaseTexture(registration->second.Managed, mLastSubmission);
		mUiTextureRegistry->Registrations.erase(registration);
	}

#if defined(HE_DEV_AGENT_RUNTIME)
    bool Application::InjectUpscalingFailureForValidation(bool duringDispatch)
    {
        return mRuntimeRenderer && mRuntimeRenderer->InjectUpscalingFailureForValidation(duringDispatch);
    }
#endif
	size_t Application::RequestTextureReload(std::string_view assetPath)
	{
		return mRuntimeRenderer && mRuntimeRenderer->IsInitialized()
			? mRuntimeRenderer->RequestTextureReload(assetPath) : 0;
	}

#if defined(HE_DEV_AGENT_RUNTIME)
	TextureReadyProbeResult Application::ProbeTextureReady(
		std::string_view assetPath, uint32_t maximumDimension,
		bool bSrgb, bool bGenerateMipmaps,
		bool bPreserveAlphaCoverage, float alphaCoverageCutoff,
		bool bReadbackMips)
	{
		if (!mRuntimeRenderer || !mRuntimeRenderer->IsInitialized())
			throw std::logic_error("Texture ready probe requires an initialized runtime renderer");
		TextureLoadDesc desc{
			.AssetPath = std::string(assetPath),
			.ColorSpace = bSrgb ? TextureColorSpace::SRgb : TextureColorSpace::Linear,
			.bCreateBindlessDescriptor = false,
			.bGenerateMipmaps = bGenerateMipmaps,
			.bPreserveAlphaCoverage = bPreserveAlphaCoverage,
			.AlphaCoverageCutoff = alphaCoverageCutoff,
			.MaximumDimension = maximumDimension,
			.bRequireReady = false,
			.bAllowReadback = bReadbackMips,
			.DebugName = "Agent texture ready probe",
		};
		ManagedTextureHandle pending;
		ManagedTextureHandle ready;
		const auto release = [&]
		{
			if (ready)
			{
				const ManagedTextureHandle handle = ready;
				ready = {};
				mRuntimeRenderer->ReleaseTexture(handle, mLastSubmission);
			}
			if (pending)
			{
				const ManagedTextureHandle handle = pending;
				pending = {};
				mRuntimeRenderer->ReleaseTexture(handle, mLastSubmission);
			}
		};
		try
		{
			pending = mRuntimeRenderer->LoadTexture(desc);
			desc.bRequireReady = true;
			ready = mRuntimeRenderer->LoadTexture(desc);
			const ManagedTexture& resource = mRuntimeRenderer->GetTexture(ready);
			TextureReadyProbeResult result{
				.SharedHandle = pending == ready,
				.Width = resource.Width,
				.Height = resource.Height,
				.MipLevels = resource.MipLevels,
				.FormatName = nvrhi::getFormatInfo(resource.Format).name,
			};
			if (bReadbackMips)
			{
				if (resource.Format != nvrhi::Format::RGBA8_UNORM &&
					resource.Format != nvrhi::Format::SRGBA8_UNORM)
					throw std::invalid_argument("Mip readback probe requires an RGBA8 texture");
				constexpr uint64_t maxReadbackBytes = 16ull * 1024ull * 1024ull;
				uint64_t readbackBytes = 0;
				uint32_t width = resource.Width;
				uint32_t height = resource.Height;
				for (uint32_t mip = 0; mip < resource.MipLevels; ++mip)
				{
					if (height == 0 || width > maxReadbackBytes / 4u / height)
						throw std::invalid_argument("Mip readback probe exceeds 16 MiB");
					const uint64_t bytes = static_cast<uint64_t>(width) * height * 4u;
					if (bytes > maxReadbackBytes - readbackBytes)
						throw std::invalid_argument("Mip readback probe exceeds 16 MiB");
					readbackBytes += bytes;
					width = std::max(width / 2u, 1u);
					height = std::max(height / 2u, 1u);
				}
				auto* device = mRuntimeRenderer->GetDevice();
				std::vector<nvrhi::StagingTextureHandle> staging;
				staging.reserve(resource.MipLevels);
				auto commands = device->createCommandList();
				if (!commands) throw std::runtime_error("Mip readback command list creation failed");
				commands->open();
				width = resource.Width;
				height = resource.Height;
				for (uint32_t mip = 0; mip < resource.MipLevels; ++mip)
				{
					nvrhi::TextureDesc stagingDesc;
					stagingDesc.width = width;
					stagingDesc.height = height;
					stagingDesc.format = resource.Format;
					stagingDesc.debugName = "Agent texture mip readback";
					auto target = device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
					if (!target) throw std::runtime_error("Mip readback staging creation failed");
					nvrhi::TextureSlice slice;
					slice.setSize(width, height, 1);
					auto sourceSlice = slice;
					sourceSlice.setMipLevel(mip);
					commands->copyTexture(target, slice, resource.Texture, sourceSlice);
					staging.push_back(std::move(target));
					width = std::max(width / 2u, 1u);
					height = std::max(height / 2u, 1u);
				}
				commands->close();
				auto completion = device->createEventQuery();
				if (!completion) throw std::runtime_error("Mip readback event query creation failed");
				device->resetEventQuery(completion);
				device->executeCommandList(commands, nvrhi::CommandQueue::Graphics);
				device->setEventQuery(completion, nvrhi::CommandQueue::Graphics);
				device->waitEventQuery(completion);
				const float alphaThreshold = std::clamp(alphaCoverageCutoff, 0.0f, 1.0f) * 255.0f;
				width = resource.Width;
				height = resource.Height;
				for (const auto& target : staging)
				{
					nvrhi::TextureSlice slice;
					slice.setSize(width, height, 1);
					size_t rowPitch = 0;
					const auto* pixels = static_cast<const uint8_t*>(device->mapStagingTexture(
						target, slice, nvrhi::CpuAccessMode::Read, &rowPitch));
					if (!pixels)
						throw std::runtime_error("Mip readback mapping failed");
					if (rowPitch < static_cast<size_t>(width) * 4u)
					{
						device->unmapStagingTexture(target);
						throw std::runtime_error("Mip readback row pitch is too small");
					}
					TextureMipReadbackProbe mipResult{ .Width = width, .Height = height,
						.Hash = 14695981039346656037ull };
					for (uint32_t row = 0; row < height; ++row)
					{
						const uint8_t* rowPixels = pixels + static_cast<size_t>(row) * rowPitch;
						for (size_t byte = 0; byte < static_cast<size_t>(width) * 4u; ++byte)
						{
							mipResult.Hash ^= rowPixels[byte];
							mipResult.Hash *= 1099511628211ull;
						}
						for (size_t pixel = 0; pixel < width; ++pixel)
							mipResult.CoveredPixels += rowPixels[pixel * 4u + 3u] >= alphaThreshold;
					}
					device->unmapStagingTexture(target);
					result.MipReadback.push_back(mipResult);
					width = std::max(width / 2u, 1u);
					height = std::max(height / 2u, 1u);
				}
			}
			release();
			return result;
		}
		catch (...)
		{
			release();
			throw;
		}
	}

	TextureBurstProbeResult Application::ProbeTextureBurst(
		std::string_view assetPath, uint32_t count)
	{
		if (!mRuntimeRenderer || !mRuntimeRenderer->IsInitialized())
			throw std::logic_error("Texture burst probe requires an initialized runtime renderer");
		if (count == 0 || count > 96)
			throw std::invalid_argument("Texture burst probe count must be 1..96");
		std::vector<ManagedTextureHandle> pending;
		pending.reserve(count);
		const auto releasePending = [&]
		{
			while (!pending.empty())
			{
				const ManagedTextureHandle handle = pending.back();
				pending.pop_back();
				mRuntimeRenderer->ReleaseTexture(handle, mLastSubmission);
			}
		};
		TextureBurstProbeResult result;
		try
		{
			TextureLoadDesc desc{
				.AssetPath = std::string(assetPath),
				.ColorSpace = TextureColorSpace::Linear,
				.bCreateBindlessDescriptor = false,
				.bGenerateMipmaps = false,
				.bRequireReady = false,
				.DebugName = "Agent texture burst probe",
			};
			for (uint32_t index = 0; index < count; ++index)
			{
				// MaximumDimension participates in the cache key. Request all
				// variants before waiting so the bounded preparation queue fills.
				desc.MaximumDimension = 64 + index;
				pending.push_back(mRuntimeRenderer->LoadTexture(desc));
				++result.Requested;
			}
			desc.bRequireReady = true;
			for (uint32_t index = 0; index < count; ++index)
			{
				desc.MaximumDimension = 64 + index;
				ManagedTextureHandle ready = mRuntimeRenderer->LoadTexture(desc);
				try
				{
					result.Shared += ready == pending[index];
					const ManagedTexture& resource = mRuntimeRenderer->GetTexture(ready);
					if (resource.Width == 0 || resource.Height == 0)
						throw std::runtime_error("Texture burst probe published an empty texture");
					++result.Ready;
				}
				catch (...)
				{
					mRuntimeRenderer->ReleaseTexture(ready, mLastSubmission);
					throw;
				}
				mRuntimeRenderer->ReleaseTexture(ready, mLastSubmission);
			}
			releasePending();
			return result;
		}
		catch (...)
		{
			releasePending();
			throw;
		}
	}

	TextureStreamingProbeResult Application::BeginTextureStreamingProbe(
		std::string_view assetPath, uint32_t count,
		uint32_t baseDimension)
	{
		if (!mRuntimeRenderer || !mRuntimeRenderer->IsInitialized())
			throw std::logic_error("Texture streaming probe requires an initialized runtime renderer");
		if (!mTextureStreamingProbe.empty())
			throw std::logic_error("Texture streaming probe is already active");
		if (count == 0 || count > 96)
			throw std::invalid_argument("Texture streaming probe count must be 1..96");
		if (baseDimension < 32 || baseDimension > 2048 ||
			count - 1 > 2048 - baseDimension)
			throw std::invalid_argument("Texture streaming probe dimension range must fit 32..2048");
		mTextureStreamingProbe.reserve(count);
		TextureLoadDesc desc{
			.AssetPath = std::string(assetPath),
			.ColorSpace = TextureColorSpace::Linear,
			.bCreateBindlessDescriptor = false,
			.bGenerateMipmaps = false,
			.bRequireReady = false,
			.DebugName = "Agent texture streaming probe",
		};
		try
		{
			for (uint32_t index = 0; index < count; ++index)
			{
				desc.MaximumDimension = baseDimension + index;
				mTextureStreamingProbe.push_back(mRuntimeRenderer->LoadTexture(desc));
			}
		}
		catch (...)
		{
			(void)InspectTextureStreamingProbe(true);
			throw;
		}
		return InspectTextureStreamingProbe(false);
	}

	TextureStreamingProbeResult Application::InspectTextureStreamingProbe(
		bool bRelease)
	{
		TextureStreamingProbeResult result;
		result.Requested = static_cast<uint32_t>(mTextureStreamingProbe.size());
		if (!mRuntimeRenderer || !mRuntimeRenderer->IsInitialized())
			throw std::logic_error("Texture streaming probe renderer is unavailable");
		for (const ManagedTextureHandle handle : mTextureStreamingProbe)
		{
			const ManagedTexture& resource = mRuntimeRenderer->GetTexture(handle);
			result.Ready += resource.Width != 0 && resource.Height != 0;
		}
		result.Pending = result.Requested - result.Ready;
		if (bRelease)
		{
			while (!mTextureStreamingProbe.empty())
			{
				const ManagedTextureHandle handle = mTextureStreamingProbe.back();
				mTextureStreamingProbe.pop_back();
				mRuntimeRenderer->ReleaseTexture(handle, mLastSubmission);
			}
		}
		return result;
	}
#endif

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

    const SceneRendererStats& Application::GetMainSceneStats() const
    {
        static const SceneRendererStats empty;
        return mRuntimeRenderer ? mRuntimeRenderer->GetSceneStats() : empty;
    }

    RenderMemoryBudget Application::GetRenderMemoryBudget() const
    {
        return mRuntimeRenderer ? mRuntimeRenderer->QueryMemoryBudget() : RenderMemoryBudget{};
    }

	const SceneRendererStats& Application::GetOffscreenSceneStats(RuntimeOffscreenViewHandle handle) const
	{
		if (!mRuntimeRenderer) throw std::logic_error("Offscreen statistics require RuntimeRenderer");
		return mRuntimeRenderer->GetOffscreenSceneStats(handle);
	}

    RuntimeConfigurationPreparationStats Application::GetConfigurationPreparationStats() const noexcept
    {
        return mRuntimeRenderer ? mRuntimeRenderer->GetConfigurationPreparationStats()
            : RuntimeConfigurationPreparationStats{};
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

    bool Application::IsFrameCaptureSourceAvailable(FrameCaptureSource source) const
    {
        return source == FrameCaptureSource::Automatic ||
            (mRuntimeRenderer && mRuntimeRenderer->IsCaptureSourceAvailable(source));
    }

	bool Application::RequestFrameCapture(std::filesystem::path outputPath, FrameCaptureSource source)
	{
		if (!mRuntimeRenderer || outputPath.empty() ||
			mFrameCaptureProgress.State != RuntimeFrameCaptureState::Idle)
		{
			return false;
		}

        if (source != FrameCaptureSource::Automatic && outputPath.extension() != ".raw")
            throw std::invalid_argument("Explicit native capture sources require .raw output");
        if (!IsFrameCaptureSourceAvailable(source)) throw CaptureSourceUnavailable(source);
		mFrameCaptureProgress = {
			.State = RuntimeFrameCaptureState::Queued,
			.OutputPath = std::move(outputPath),
            .Source = source,
		};
		return true;
	}

    bool Application::RequestFrameCaptureSequence(std::vector<std::filesystem::path> paths, bool bCameraCut, uint32_t settleFrames,
        FrameCaptureSource source)
    {
        if (settleFrames > 600 || (!bCameraCut && settleFrames != 0) ||
            paths.size() < 8 || paths.size() > 16 || !mRuntimeRenderer ||
            mFrameCaptureProgress.State != RuntimeFrameCaptureState::Idle) return false;
        if (!IsFrameCaptureSourceAvailable(source)) throw CaptureSourceUnavailable(source);
        std::vector<RuntimeFrameCaptureProgress> entries;
        for (auto& path : paths)
        {
            if (path.empty()) return false;
            if (source != FrameCaptureSource::Automatic && path.extension() != ".raw")
                throw std::invalid_argument("Explicit native capture sources require .raw output");
            entries.push_back({ .State = RuntimeFrameCaptureState::Queued, .OutputPath = std::move(path), .Source = source });
        }
        // Allocate all sequence storage before the caller performs its noexcept camera cut.
        std::vector<TextureReadbackHandle> handles(entries.size());
        Json history = Json::array();
        history.get_ref<Json::array_t&>().reserve(settleFrames + entries.size());
        mSequenceCaptures = std::move(entries);
        mSequenceHandles = std::move(handles);
        bSequenceCameraCut = bCameraCut;
        mSequenceSettleRemaining = settleFrames;
        mSequenceSettleSimulationStep = mCaptureSimulationStep;
        mSequenceHistory = std::move(history);
        mSequenceRecorded = 0;
        mFrameCaptureProgress = mSequenceCaptures.front();
        return true;
    }

    bool Application::CanAdvanceCaptureSequence() const
    {
        return !mSequenceCaptures.empty() && mSequenceSettleRemaining == 0 &&
            mFrameCaptureProgress.State == RuntimeFrameCaptureState::Queued;
    }

    void Application::CancelFrameCaptureSequence()
    {
        mSequenceSettleRemaining = 0;
        std::string reason = mFrameCaptureProgress.Error;
        if (reason.empty())
            for (const auto& entry : mSequenceCaptures)
                if (entry.State == RuntimeFrameCaptureState::Failed && !entry.Error.empty())
                { reason = entry.Error; break; }
        if (reason.empty()) reason = "Capture sequence cancelled";
        for (auto& handle : mSequenceHandles)
        {
            if (handle && mRuntimeRenderer && mRuntimeRenderer->IsCaptureValid(handle))
                mRuntimeRenderer->ReleaseCapture(handle); // In-flight releases remain fence-retired.
            handle = {};
        }
        for (auto& entry : mSequenceCaptures)
            if (entry.State == RuntimeFrameCaptureState::Queued || entry.State == RuntimeFrameCaptureState::InFlight)
            {
                entry.Error = entry.State == RuntimeFrameCaptureState::Queued
                    ? "Not recorded: " + reason : "Readback cancelled: " + reason;
                entry.State = RuntimeFrameCaptureState::Failed;
            }
        if (!mSequenceCaptures.empty())
        {
            mFrameCaptureProgress.State = RuntimeFrameCaptureState::Failed;
            mFrameCaptureProgress.Error = std::move(reason);
        }
    }

	void Application::ClearFrameCapture()
	{
		if (mFrameCaptureProgress.State == RuntimeFrameCaptureState::Queued ||
			mFrameCaptureProgress.State == RuntimeFrameCaptureState::InFlight)
		{
			throw std::logic_error("Cannot clear an unfinished runtime frame capture");
		}
        // Failure may originate outside PollFrameCapture. Never discard live
        // readback handles when a controller clears the settled ticket.
        if (!mSequenceCaptures.empty() && mFrameCaptureProgress.State == RuntimeFrameCaptureState::Failed)
            CancelFrameCaptureSequence();
        for (auto& handle : mSequenceHandles)
            if (handle && mRuntimeRenderer && mRuntimeRenderer->IsCaptureValid(handle))
                mRuntimeRenderer->ReleaseCapture(handle);
		mFrameCaptureHandle = {};
		mFrameCaptureProgress = {};
        mSequenceCaptures.clear();
        mSequenceHandles.clear();
        mSequenceRecorded = 0;
        bSequenceCameraCut = false;
        mSequenceSettleRemaining = 0;
        mSequenceHistory = Json::array();
	}

	void Application::PollFrameCapture()
	{
        if (!mSequenceCaptures.empty())
        {
            bool failed = false;
            size_t complete = 0;
            for (size_t index = 0; index < mSequenceCaptures.size(); ++index)
            {
                auto& entry = mSequenceCaptures[index];
                auto& handle = mSequenceHandles[index];
                if (handle)
                {
                    if (!mRuntimeRenderer->IsCaptureValid(handle))
                    { entry.State = RuntimeFrameCaptureState::Failed; entry.Error = "Capture handle invalidated"; handle = {}; }
                    else
                    {
                        const auto result = mRuntimeRenderer->GetCaptureResult(handle);
                        if (result.Status == TextureReadbackStatus::Complete || result.Status == TextureReadbackStatus::Failed)
                        {
                            entry.State = result.Status == TextureReadbackStatus::Complete ? RuntimeFrameCaptureState::Complete : RuntimeFrameCaptureState::Failed;
                            entry.Error = result.Error;
                            mRuntimeRenderer->ReleaseCapture(handle);
                            handle = {};
                        }
                    }
                }
                failed |= entry.State == RuntimeFrameCaptureState::Failed;
                complete += entry.State == RuntimeFrameCaptureState::Complete ? 1 : 0;
            }
            if (failed) CancelFrameCaptureSequence();
            else if (complete == mSequenceCaptures.size()) mFrameCaptureProgress.State = RuntimeFrameCaptureState::Complete;
            return;
        }
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
        CancelFrameCaptureSequence();
        mSequenceCaptures.clear();
        mSequenceHandles.clear();
        mSequenceRecorded = 0;
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

		// Asset warmup can present a variable number of zero-timestep frames.
		// Start gameplay and benchmark sampling from one reproducible history epoch.
		if (mRuntimeRenderer)
			mRuntimeRenderer->ResetTemporalHistory();
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
		return SimulationClock::GetInstance().GetFixedElapsedSeconds();
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
		// UI gets first refusal; releases still reach the script pointer latch.
		InputActions::Get().ObserveEvent(e);
		ScriptPlatformServices::ObserveEvent(e);
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
		if (!mRunOptions.RenderSettingsPath.empty())
		{
			if (!mRunOptions.RenderSettingsPath.is_absolute() ||
				!std::filesystem::is_regular_file(mRunOptions.RenderSettingsPath) ||
				std::filesystem::file_size(mRunOptions.RenderSettingsPath) > 1024u * 1024u)
				throw std::invalid_argument("Render settings require an existing absolute JSON file of at most 1 MiB");
			std::ifstream input(mRunOptions.RenderSettingsPath);
			if (!input) throw std::runtime_error("Could not open render settings JSON");
			commandLineRenderOverrides = Json::parse(input);
			if (!commandLineRenderOverrides.is_object())
				throw std::invalid_argument("Render settings JSON must be a flat stable-ID patch object");
			for (const auto& [id, value] : commandLineRenderOverrides.items())
			{
				(void)value;
				if (!FindRenderSettingDescriptor(id))
					throw std::invalid_argument("Unknown render settings JSON property: " + id);
			}
		}
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
		if (mRunOptions.RenderingPathOverride)
			commandLineRenderOverrides[std::string(RenderSettingIds::Path)] =
				*mRunOptions.RenderingPathOverride;
        // --render-path selects the initial path; it must not permanently mask
        // later frame-boundary Session transactions. Other CLI constraints keep
        // their established higher-priority semantics (including the backend).
        Json startupPath = nullptr;
        if (const auto path = commandLineRenderOverrides.find(std::string(RenderSettingIds::Path));
            path != commandLineRenderOverrides.end())
        {
            startupPath = *path;
            commandLineRenderOverrides.erase(path);
        }
		if (!commandLineRenderOverrides.empty())
			renderSettingsService.SetCommandLineOverrides(
				commandLineRenderOverrides);
        if (!startupPath.is_null())
        {
            auto draft = renderSettingsService.CreateDraft(SettingsLayer::Session);
            draft.Patch[std::string(RenderSettingIds::Path)] = std::move(startupPath);
            const auto applied = renderSettingsService.ApplyDraft(draft, false);
            if (!applied.bSuccess)
                throw std::invalid_argument("Invalid startup rendering path: " + applied.Message);
        }
		const ResolvedRenderSettings initialRenderSettings =
			renderSettingsService.GetSnapshot();
		for (const auto& diagnostic : initialRenderSettings.Diagnostics)
			if (diagnostic.Severity == RenderSettingDiagnosticSeverity::Error)
				throw std::invalid_argument("Invalid render settings: " + diagnostic.PropertyId + ": " + diagnostic.Message);

		WindowProps windowProps(name);
		windowProps.bVSync = initialRenderSettings.Effective.bVSync;
#if !defined(WITH_EDITOR)
		// Gameplay has no text fields. Disable the Windows IME before GLFW creates
		// the first top-level window so TSF cannot turn movement keys into VK_PROCESSKEY.
		windowProps.bEnableInputMethod = false;
        windowProps.bFullscreen = LoadGameStartupFullscreen(
            mRunOptions.FullscreenOverride, mRunOptions.HasFixedWindowSize());
        windowProps.bLockFullscreen = mRunOptions.FullscreenOverride.has_value();
#endif
		if (mRunOptions.HasFixedWindowSize())
		{
			windowProps.Width = mRunOptions.WindowWidth;
			windowProps.Height = mRunOptions.WindowHeight;
			windowProps.bMaximized = false;
			windowProps.bResizable = false;
			windowProps.bAllowFullscreen = false;
            windowProps.bFullscreen = false;
			windowProps.bExactClientSize = true;
		}
		mWindow = Window::Create(windowProps);
		mWindow->SetEventCallback(HE_BIND_EVENT_FN(Application::OnEvent));

		auto* renderSurface = dynamic_cast<RenderSurface*>(mWindow.get());
		if (!renderSurface)
			throw std::runtime_error("The active Window does not provide a RenderSurface");

		const std::filesystem::path shaderCache =
			ShaderArtifactResolver(
                ConfigManager::GetEngineGeneratedContentFullPath("Shaders/CACHES"),
                initialRenderSettings.Effective.Backend).GetArtifactDirectory();
		const AntiAliasingSettings antiAliasing =
			renderSettingsService.BuildRequestedRuntimeAntiAliasing();
		const GlobalIlluminationSettings globalIllumination =
			renderSettingsService.BuildRequestedRuntimeGlobalIllumination();
		RuntimeRendererDesc rendererDesc{
            .PipelineType = initialRenderSettings.Effective.PipelineType,
            .PipelineFeatures = initialRenderSettings.Effective.PipelineFeatures,
            .TransientCacheBudgetBytes = initialRenderSettings.Effective.TransientCacheBudgetMiB == 0
                ? std::numeric_limits<uint64_t>::max()
                : uint64_t(initialRenderSettings.Effective.TransientCacheBudgetMiB) * 1024u * 1024u,
            .TextureStreamingBudgetBytes = uint64_t(initialRenderSettings.Effective.TextureStreamingBudgetMiB) * 1024 * 1024,
			.Surface = renderSurface,
			.Backend = initialRenderSettings.Effective.Backend,
			.Path = initialRenderSettings.Effective.Path,
			.AdapterSelection = {
				.bUseWarp = mRunOptions.bUseWarp,
			},
            .FramesInFlight = initialRenderSettings.Effective.FramesInFlight,
			.ShaderCacheDirectory = shaderCache,
			.AssetRootDirectory = ConfigManager::GetInstance().GetRootFolder(),
			.EngineAssetRootDirectory = ConfigManager::GetInstance().GetEngineContentFolder(),
			// Validation is intentionally opt-in. The DX12 debug layer and NVRHI
			// validation are indispensable diagnostics, but are not a representative
			// editor-performance configuration.
			.bEnableValidation =
				initialRenderSettings.Debug.bRenderGraphValidation,
			.bEnableGpuTiming = mRunOptions.bRenderStats,
            .bEnableCpuGraphTiming = mRunOptions.bRenderStats,
            // Process-local A/B control; production defaults retain full-frame timing.
            .bEnableGpuFrameMarkers = [] {
                const char* value = std::getenv("HE_DISABLE_GPU_FRAME_MARKERS");
                return !(value && value[0] == '1' && value[1] == '\0');
            }(),
			.GlobalIllumination = globalIllumination,
			.AntiAliasing = antiAliasing,
			.Upscaling = initialRenderSettings.Effective.Upscaling,
			.PostProcess = initialRenderSettings.Effective.PostProcess,
			.AmbientOcclusion = initialRenderSettings.Effective.AmbientOcclusion,
            .Clouds = initialRenderSettings.Effective.Clouds,
			.Reflections = initialRenderSettings.Effective.Reflections,
			.bGpuOcclusionEnabled = initialRenderSettings.Effective.bGpuOcclusionEnabled,
			.bGpuBatchingEnabled = initialRenderSettings.Effective.bGpuBatchingEnabled,
			.bGpuTightFrustumEnabled = initialRenderSettings.Effective.bGpuTightFrustumEnabled,
            .bGpuShadowBatchingEnabled = initialRenderSettings.Effective.bGpuShadowBatchingEnabled,
            .bTransientTextureReuse = initialRenderSettings.Effective.bTransientTextureReuse,
			.bLodEnabled = initialRenderSettings.Effective.bLodEnabled,
			.bShadowsEnabled = initialRenderSettings.Effective.Shadows.bEnabled,
            .bContinuousPcfEnabled = initialRenderSettings.Effective.Shadows.bContinuousPcfEnabled,
			.LodMaxPixelError = initialRenderSettings.Effective.LodMaxPixelError,
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

		WriteRenderingSettingsEvidence(mRunOptions, *mRuntimeRenderer, *mWindow, "startup");

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
		resourceRegistry.Init(GetRuntimeCpuBudget().ResourceWorkers);
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
		std::uint64_t measuredFrameCount = 0;
		double measuredFrameSeconds = 0.0;
        uint64_t cpuTimingFrameId = 0;
        struct GpuTimingFrameGuard
        {
            RuntimeRenderer* Renderer;
            ~GpuTimingFrameGuard() { if (Renderer) Renderer->CancelGpuTimingFrame(); }
        };
		struct PerformanceFrame
		{
			std::uint64_t Frame;
			double WallMilliseconds;
			double UpdateMilliseconds;
			double RenderSubmitMilliseconds;
			double WindowMilliseconds;
			std::uint32_t Width;
			std::uint32_t Height;
			std::uint64_t GpuSample;
            uint64_t SubmissionTimingSampleId;
            uint64_t CpuTimingFrameId;
			double GpuPassSumMilliseconds;
			double GpuFrameSpanMilliseconds;
			std::uint64_t SceneTableUploadedBytes;
			std::uint32_t Objects;
			std::uint32_t Visible;
			std::uint32_t Batches;
			std::uint32_t ShadowDraws;
			std::uint64_t TransientReservedBytes;
			RenderFrameCpuTimings BackendCpuTimings;
            std::vector<SceneGpuPassTiming> GpuPassTimings;
            uint64_t GpuVisibleTriangles = 0;
            uint64_t GpuCullingReadbackSubmission = 0;
            uint32_t GpuCullingReadbackInputObjects = 0;
            uint32_t GpuCullingReadbackBatches = 0;
            uint64_t BaseRasterTriangles = 0;
            uint64_t SelectedRasterTriangles = 0;
            uint32_t ReducedLodSections = 0;
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
            double PreUpdateMilliseconds = 0.0;
            double CloudCpuRecordMilliseconds = 0.0;
            double CpuFramePreparationMilliseconds = 0.0;
            double CpuMaterialPackingMilliseconds = 0.0;
            uint32_t MaterialPackingCallCount = 0;
            uint32_t MaterialPackingCacheHitCount = 0;
            bool bMaterialPackingCacheEnabled = true;
            uint64_t RenderGraphCachedTransientBytes = 0;
            uint64_t RenderGraphRetiredResourceBytes = 0;
            uint64_t RenderGraphTrimmedTransientBytes = 0;
            uint32_t CustomGpuDraws = 0;
            uint32_t CustomGpuObjects = 0;
            double CpuGraphRecordMilliseconds = 0.0;
            double CpuGraphBuildMilliseconds = 0.0;
            uint32_t GraphBuildReasonMask = 0;
            bool bRuntimeSceneLoading = false;
            uint32_t PendingTextureUploads = 0;
            uint32_t InternalWidth = 0;
            uint32_t InternalHeight = 0;
            bool bCpuCounterFrameValid = false;
            uint64_t CpuCounterWorldFrame = 0;
            uint64_t InputRenderObjects = 0;
            uint64_t PreparedRasterSections = 0;
            uint64_t SectionsWithAuthoredLods = 0;
            std::array<uint64_t, 9> SelectedLodSections{};
            std::array<uint64_t, 4> DirectionalCpuCasterDraws{};
            uint32_t LocalShadowActiveViews = 0;
            uint32_t LocalShadowDirtyViews = 0;
            uint32_t LocalShadowCachedViews = 0;
            uint32_t LocalShadowVisibleCasters = 0;
            GlobalIlluminationStats GiBudget;
            uint64_t CpuThreadId = 0;
            double CpuStartUtcMilliseconds = 0.0;
            double CpuStartIntervalMilliseconds = std::numeric_limits<double>::quiet_NaN();
            double CpuThreadBusyMilliseconds = std::numeric_limits<double>::quiet_NaN();
            double CpuProcessBusyMilliseconds = std::numeric_limits<double>::quiet_NaN();
            bool bCpuIntervalValid = false;
            bool bCpuBusyValid = false;
            uint64_t GraphSubmissionId = 0;
            AnimationCpuTimings AnimationCpu;
            double CpuStatsPrologueMilliseconds = 0.0;
            double CpuTimingBeginMilliseconds = 0.0;
            double CpuResourcePumpMilliseconds = 0.0;
            double CpuAudioInputMilliseconds = 0.0;
            double CpuCapturePollMilliseconds = 0.0;
            double CpuSubmissionCollectMilliseconds = 0.0;
            double CpuMemorySampleMilliseconds = 0.0;
            double CpuStatsQueryMilliseconds = 0.0;
            double CpuStatsBufferMilliseconds = 0.0;
            double CpuImGuiMilliseconds = 0.0;
            double CpuRuntimeRenderMilliseconds = 0.0;
		};
		std::optional<PerformanceFrame> pendingPerformanceFrame;
        PerformanceFrame* previousPerformanceFrame = nullptr;
        struct PerformanceMemorySample
        {
            uint64_t Frame = 0;
            double ElapsedSeconds = 0.0;
            double QueryMilliseconds = 0.0;
            RenderMemoryBudget Budget;
            bool bAutomationFrame = false;
        };
        bool bHasMemorySample = false;
        BoundedStatisticsBuffer<PerformanceFrame> performanceFrames;
        BoundedStatisticsBuffer<PerformanceMemorySample> memorySamples;
        BoundedStatisticsBuffer<RenderSubmissionGpuTiming> submissionSamples;
        const auto memorySampleStart = std::chrono::steady_clock::now();
        auto lastMemorySample = memorySampleStart;
		std::uint64_t gpuWarmupCutoffSample = 0;
        struct CpuGraphCsvRow
        {
            uint64_t CpuFrame, WorldFrame, Submission;
            double Milliseconds;
            uint32_t NameIndex, ExecutionIndex;
            uint8_t Phase;
            bool bCompleted;
        };
        // All statistics use bounded capture storage; CSV formatting is deferred until shutdown.
        std::vector<CpuGraphCsvRow> cpuGraphRows;
        std::unordered_map<std::string, uint32_t> cpuGraphNameIds;
        std::vector<std::string> cpuGraphNames;
        size_t cpuGraphRowLimit = 0;
        std::ofstream csv, submissionCsv, passCsv, cpuPassCsv, memoryCsv, wallSamples;
        std::filesystem::path wallSamplesPath;
        uint64_t writtenFrames = 0, writtenSubmissions = 0, lastWrittenSample = 0;
        uint64_t overBudget60 = 0, overBudget90 = 0, consecutiveOverBudget60 = 0, longestOverBudget60 = 0;
        if (mRunOptions.bRenderStats)
        {
            if (mRunOptions.RenderStatsPath.empty())
            {
                const auto identity = std::chrono::system_clock::now().time_since_epoch().count();
                mRunOptions.RenderStatsPath = ConfigManager::GetSavedPath(
                    "RenderStats/frames-" + std::to_string(identity) + ".csv");
            }
            constexpr size_t maxCpuGraphRows = 4'000'000;
            constexpr size_t expectedRowsPerFrame = 256;
            const uint64_t requestedFrames = mRunOptions.ExitAfterFrames;
            constexpr size_t maxStatisticsFrames = 100'000;
            if (requestedFrames > maxStatisticsFrames)
                throw std::runtime_error("Render statistics frame request exceeds the bounded capture limit");
            const size_t statisticsFrameLimit = requestedFrames > 0
                ? static_cast<size_t>(requestedFrames) : 16'384;
            performanceFrames.Reserve(statisticsFrameLimit);
            memorySamples.Reserve(statisticsFrameLimit + 4'096);
            submissionSamples.Reserve(statisticsFrameLimit + 4'096);
            HE_CORE_INFO("[RenderStatistics] Structured capacity frames={}x{} bytes, submissions={}x{}, memory={}x{}; owned snapshot payloads additional",
                performanceFrames.Limit(), sizeof(PerformanceFrame), submissionSamples.Limit(), sizeof(RenderSubmissionGpuTiming),
                memorySamples.Limit(), sizeof(PerformanceMemorySample));
            cpuGraphRowLimit = requestedFrames > 0 && requestedFrames <= maxCpuGraphRows / expectedRowsPerFrame
                ? static_cast<size_t>(requestedFrames) * expectedRowsPerFrame : maxCpuGraphRows;
            cpuGraphRows.reserve(cpuGraphRowLimit);
            cpuGraphNameIds.reserve(256);
            cpuGraphNames.reserve(256);
            HE_CORE_INFO("[RenderStatistics] CPU graph buffer capacity={} rows, {} bytes; overflow fails explicitly",
                cpuGraphRowLimit, cpuGraphRowLimit * sizeof(CpuGraphCsvRow));
            auto submissionPath = mRunOptions.RenderStatsPath; submissionPath += ".submissions.csv";
            auto passPath = mRunOptions.RenderStatsPath; passPath += ".passes.csv";
            auto cpuPassPath = mRunOptions.RenderStatsPath; cpuPassPath += ".cpu-passes.csv";
            auto memoryPath = mRunOptions.RenderStatsPath; memoryPath += ".memory.csv";
            wallSamplesPath = mRunOptions.RenderStatsPath; wallSamplesPath += ".wall-ms.bin";
            const std::array paths{mRunOptions.RenderStatsPath, submissionPath, passPath, cpuPassPath, memoryPath, wallSamplesPath};
            for (const auto& path : paths)
                if (std::filesystem::exists(path)) throw std::runtime_error("Refusing to overwrite render statistics: " + Utils::PathToUtf8(path));
            const auto openNew = [](std::ofstream& stream, const std::filesystem::path& path) {
                std::filesystem::create_directories(path.parent_path());
                Utils::WriteTextToFileAtomically(path, "", Utils::AtomicWriteMode::CreateNew);
                stream.exceptions(std::ios::badbit | std::ios::failbit);
                stream.open(path, std::ios::binary | std::ios::app);
                stream.imbue(std::locale::classic());
            };
            openNew(csv, mRunOptions.RenderStatsPath); openNew(submissionCsv, submissionPath);
            openNew(passCsv, passPath); openNew(cpuPassCsv, cpuPassPath); openNew(memoryCsv, memoryPath); openNew(wallSamples, wallSamplesPath);
            csv << "frame,cpuWallFrameMs,updateMs,renderSubmitPresentMs,windowMs,width,height,"
					"gpuSample,submissionTimingSampleId,cpuTimingFrameId,gpuPassSumMs,gpuFrameSpanMs,sceneTableUploadedBytes,gpuWarmupCutoffSample,"
					"gpuDrivenObjects,gpuVisibleObjects,gpuBatches,shadowDraws,transientReservedBytes,"
					"cpuAcquireImageMs,cpuFrameSlotWaitMs,cpuGraphicsSubmitMs,cpuPresentMs,"
					"cpuGarbageCollectMs,cpuCommandListSetupMs,cpuRenderOtherMs,"
                    "gpuVisibleTriangles,gpuCullingReadbackSubmission,gpuCullingReadbackInputObjects,gpuCullingReadbackBatches,baseRasterTriangles,selectedRasterTriangles,reducedLodSections,"
                    "geometryArenaReservedBytes,geometryArenaUsedBytes,indirectCompactionResidentBytes,"
                    "indirectCompactionUploadedBytes,gpuMultiDrawGroups,"
                    "gpuShadowIndirectDraws,gpuShadowMultiDrawGroups,gpuShadowRecordedViews,"
                    "gpuShadowCompactedViews,gpuShadowResidentBytes,gpuShadowUploadedBytes,cpuPreUpdateMs,cpuCloudRecordMs,"
                    "cpuFramePreparationMs,cpuMaterialPackingMs,materialPackingCalls,materialPackingCacheHits,"
                    "materialPackingCacheEnabled,renderGraphCachedTransientBytes,renderGraphRetiredResourceBytes,"
                    "renderGraphTrimmedTransientBytes,customGpuDraws,customGpuObjects,cpuGraphRecordMs,cpuGraphBuildMs,graphBuildReasonMask,"
                    "cpuCountersValid,counterWorldFrame,inputRenderObjects,preparedRasterSections,sectionsWithAuthoredLods,selectedLod0Sections,selectedLod1Sections,selectedLod2Sections,selectedLod3Sections,selectedLod4Sections,selectedLod5Sections,selectedLod6Sections,selectedLod7Sections,selectedLod8Sections,directionalCpuCascade0CasterDraws,directionalCpuCascade1CasterDraws,directionalCpuCascade2CasterDraws,directionalCpuCascade3CasterDraws,localShadowActiveViews,localShadowDirtyViews,localShadowCachedViews,localShadowVisibleCasters,ssgiBudgetValid,ssgiSequence,ssgiConfiguredRays,ssgiSelectedBudgetRays,ssgiTraceWidth,ssgiTraceHeight,ssgiResponseValid,ssgiResponseFrames,ssgiResponseReasons,ssgiExecutedRays,cpuThreadId,cpuStartUtcMs,cpuStartIntervalMs,cpuThreadBusyMs,cpuProcessBusyMs,cpuIntervalValid,cpuBusyValid,graphSubmissionId,cpuAnimationMs,cpuDeformationMs,cpuClothSkinMs,cpuClothSolveMs,cpuClothRenderMs,animationInstanceUpdates,deformationInstanceUpdates,deformationVertexUpdates,cpuStatsPrologueMs,cpuTimingBeginMs,cpuResourcePumpMs,cpuAudioInputMs,cpuCapturePollMs,cpuSubmissionCollectMs,cpuMemorySampleMs,cpuStatsQueryMs,cpuStatsBufferMs,cpuImGuiMs,cpuRuntimeRenderMs,runtimeLoading,pendingTextureUploads,internalWidth,internalHeight\n";
				csv << std::fixed << std::setprecision(6);
            submissionCsv << "submissionTimingSampleId,cpuTimingFrameId,beginGraphicsSubmissionId,"
                    "completedGraphicsSubmissionId,gpuFullGraphicsFrameSpanMs,markerCpuMs,markerSubmissions,status,coverage\n"
                    << std::fixed << std::setprecision(6);
            passCsv << "gpuSample,passName,milliseconds,graphSubmissionId\n" << std::fixed << std::setprecision(6);
            cpuPassCsv << "cpuTimingFrameId,worldFrame,graphSubmissionId,submissionValid,phase,executionIndex,passName,milliseconds,completed\n"
                << std::fixed << std::setprecision(6);
            memoryCsv << "frame,elapsedSeconds,available,processScope,localBudgetBytes,localUsageBytes,"
                    "localReservationBytes,nonLocalAvailable,nonLocalBudgetBytes,nonLocalUsageBytes,queryMilliseconds,automationFrame\n"
                    << std::fixed << std::setprecision(6);
            csv.flush(); submissionCsv.flush(); passCsv.flush(); cpuPassCsv.flush(); memoryCsv.flush();
            HE_CORE_INFO("[RenderStatistics] Buffered capture, files finalized after loop: {0}; submissions: {1}; passes: {2}; memory: {3}; exactWall: {4}",
                Utils::PathToUtf8(mRunOptions.RenderStatsPath), Utils::PathToUtf8(submissionPath), Utils::PathToUtf8(passPath),
                Utils::PathToUtf8(memoryPath), Utils::PathToUtf8(wallSamplesPath));
        }
        const auto collectSubmissionTimings = [&]() {
            for (auto& sample : mRuntimeRenderer->TakeSubmissionTimings())
                submissionSamples.Append(std::move(sample));
        };
        const auto writeSubmissionTimings = [&]() {
            for (const auto& sample : submissionSamples.Rows())
            {
                submissionCsv << sample.SampleId << ',' << sample.CpuFrameId << ','
                        << sample.BeginSubmissionId << ',' << sample.EndSubmissionId << ',';
                    if (sample.Status == RenderSubmissionTimingStatus::Complete)
                        submissionCsv << sample.Milliseconds;
                    submissionCsv << ',' << sample.MarkerCpuMilliseconds << ',' << sample.MarkerSubmissionCount
                        << ',' << static_cast<uint32_t>(sample.Status)
                        << ",graphics-queue-marker-through-present-transition-including-intermediate-submissions\n";
                if (++writtenSubmissions % 256u == 0) submissionCsv.flush();
            }
        };
        const auto writeMemorySample = [&](const PerformanceMemorySample& sample) {
            const auto& budget = sample.Budget;
                    memoryCsv << sample.Frame << ',' << sample.ElapsedSeconds << ',' << budget.bAvailable << ','
                        << budget.bPerProcess << ',' << budget.LocalBudgetBytes << ',' << budget.LocalUsageBytes << ','
                        << budget.LocalReservationBytes << ',' << budget.bNonLocalAvailable << ','
                        << budget.NonLocalBudgetBytes << ',' << budget.NonLocalUsageBytes << ','
                        << sample.QueryMilliseconds << ',' << sample.bAutomationFrame << '\n';
            memoryCsv.flush();
        };
        const auto writePerformanceFrame = [&](const PerformanceFrame& sample) {
            csv << sample.Frame << ',' << sample.WallMilliseconds << ','
						<< sample.UpdateMilliseconds << ',' << sample.RenderSubmitMilliseconds << ','
						<< sample.WindowMilliseconds << ',' << sample.Width << ',' << sample.Height << ','
						<< sample.GpuSample << ',' << sample.SubmissionTimingSampleId << ',' << sample.CpuTimingFrameId << ',' << sample.GpuPassSumMilliseconds << ','
						<< sample.GpuFrameSpanMilliseconds << ',' << sample.SceneTableUploadedBytes << ','
						<< gpuWarmupCutoffSample << ',' << sample.Objects << ','
						<< sample.Visible << ',' << sample.Batches << ',' << sample.ShadowDraws << ','
						<< sample.TransientReservedBytes << ','
						<< sample.BackendCpuTimings.Get(RenderFrameCpuPhase::AcquireImage) << ','
						<< sample.BackendCpuTimings.Get(RenderFrameCpuPhase::FrameSlotWait) << ','
						<< sample.BackendCpuTimings.Get(RenderFrameCpuPhase::Submit) << ','
						<< sample.BackendCpuTimings.Get(RenderFrameCpuPhase::Present) << ','
						<< sample.BackendCpuTimings.Get(RenderFrameCpuPhase::GarbageCollect) << ','
						<< sample.BackendCpuTimings.Get(RenderFrameCpuPhase::CommandListSetup) << ','
						<< std::max(0.0, sample.RenderSubmitMilliseconds - sample.BackendCpuTimings.TotalMilliseconds()) << ','
                        << sample.GpuVisibleTriangles << ',' << sample.GpuCullingReadbackSubmission << ','
                        << sample.GpuCullingReadbackInputObjects << ',' << sample.GpuCullingReadbackBatches << ','
                        << sample.BaseRasterTriangles << ','
                        << sample.SelectedRasterTriangles << ',' << sample.ReducedLodSections << ','
                        << sample.GeometryArenaReservedBytes << ',' << sample.GeometryArenaUsedBytes << ','
                        << sample.IndirectCompactionResidentBytes << ',' << sample.IndirectCompactionUploadedBytes << ','
                        << sample.GpuMultiDrawGroupCount << ',' << sample.GpuShadowIndirectDrawCount << ','
                        << sample.GpuShadowMultiDrawGroupCount << ',' << sample.GpuShadowRecordedViews << ','
                        << sample.GpuShadowCompactedViews << ',' << sample.GpuShadowResidentBytes << ','
                        << sample.GpuShadowUploadedBytes << ',' << sample.PreUpdateMilliseconds << ','
                        << sample.CloudCpuRecordMilliseconds << ',' << sample.CpuFramePreparationMilliseconds << ','
                        << sample.CpuMaterialPackingMilliseconds << ',' << sample.MaterialPackingCallCount << ','
                        << sample.MaterialPackingCacheHitCount << ',' << (sample.bMaterialPackingCacheEnabled ? 1 : 0) << ','
                        << sample.RenderGraphCachedTransientBytes << ',' << sample.RenderGraphRetiredResourceBytes << ','
                        << sample.RenderGraphTrimmedTransientBytes << ',' << sample.CustomGpuDraws << ','
                        << sample.CustomGpuObjects << ',' << sample.CpuGraphRecordMilliseconds << ','
                        << sample.CpuGraphBuildMilliseconds << ',' << sample.GraphBuildReasonMask << ','
                        << (sample.bCpuCounterFrameValid ? 1 : 0) << ','
                        << sample.CpuCounterWorldFrame << ','
                        << sample.InputRenderObjects << ','
                        << sample.PreparedRasterSections << ','
                        << sample.SectionsWithAuthoredLods << ','
                        << sample.SelectedLodSections[0] << ','
                        << sample.SelectedLodSections[1] << ','
                        << sample.SelectedLodSections[2] << ','
                        << sample.SelectedLodSections[3] << ','
                        << sample.SelectedLodSections[4] << ','
                        << sample.SelectedLodSections[5] << ','
                        << sample.SelectedLodSections[6] << ','
                        << sample.SelectedLodSections[7] << ','
                        << sample.SelectedLodSections[8] << ','
                        << sample.DirectionalCpuCasterDraws[0] << ','
                        << sample.DirectionalCpuCasterDraws[1] << ','
                        << sample.DirectionalCpuCasterDraws[2] << ','
                        << sample.DirectionalCpuCasterDraws[3] << ','
                        << sample.LocalShadowActiveViews << ','
                        << sample.LocalShadowDirtyViews << ','
                        << sample.LocalShadowCachedViews << ','
                        << sample.LocalShadowVisibleCasters << ','
                        << (sample.GiBudget.bScreenSpaceBudgetValid ? 1 : 0) << ','
                        << sample.GiBudget.FrameSequenceIndex << ','
                        << sample.GiBudget.ConfiguredScreenSpaceRays << ','
                        << sample.GiBudget.SelectedScreenSpaceBudgetRays << ','
                        << sample.GiBudget.ScreenSpaceTraceWidth << ','
                        << sample.GiBudget.ScreenSpaceTraceHeight << ','
                        << (sample.GiBudget.DenoiserBudget.bValid ? 1 : 0) << ','
                        << sample.GiBudget.DenoiserBudget.ResponseFrames << ','
                        << sample.GiBudget.DenoiserBudget.ResponseReasons << ','
                        << sample.GiBudget.ExecutedScreenSpaceRays << ','
                        << sample.CpuThreadId << ',' << sample.CpuStartUtcMilliseconds << ','
                        << sample.CpuStartIntervalMilliseconds << ',' << sample.CpuThreadBusyMilliseconds << ','
                        << sample.CpuProcessBusyMilliseconds << ',' << (sample.bCpuIntervalValid ? 1 : 0) << ','
                        << (sample.bCpuBusyValid ? 1 : 0) << ','
                        << sample.GraphSubmissionId << ',' << sample.AnimationCpu.AnimationMilliseconds << ','
                        << sample.AnimationCpu.DeformationMilliseconds << ','
                        << sample.AnimationCpu.ClothSkinMilliseconds << ','
                        << sample.AnimationCpu.ClothSolveMilliseconds << ','
                        << sample.AnimationCpu.ClothRenderMilliseconds << ','
                        << sample.AnimationCpu.InstanceUpdates << ','
                        << sample.AnimationCpu.DeformationInstanceUpdates << ',' << sample.AnimationCpu.DeformationVertexUpdates << ','
                        << sample.CpuStatsPrologueMilliseconds << ',' << sample.CpuTimingBeginMilliseconds << ','
                        << sample.CpuResourcePumpMilliseconds << ',' << sample.CpuAudioInputMilliseconds << ','
                        << sample.CpuCapturePollMilliseconds << ',' << sample.CpuSubmissionCollectMilliseconds << ','
                        << sample.CpuMemorySampleMilliseconds << ','
                        << sample.CpuStatsQueryMilliseconds << ',' << sample.CpuStatsBufferMilliseconds << ','
                        << sample.CpuImGuiMilliseconds << ',' << sample.CpuRuntimeRenderMilliseconds << ','
                        << (sample.bRuntimeSceneLoading ? 1 : 0) << ',' << sample.PendingTextureUploads << ','
                        << sample.InternalWidth << ',' << sample.InternalHeight << '\n';
            // Preserve full double precision for exact final percentiles. CSV remains six decimals.
            wallSamples.write(reinterpret_cast<const char*>(&sample.WallMilliseconds), sizeof(double));
            if (sample.GpuSample != 0 && sample.GpuSample != lastWrittenSample)
            {
                lastWrittenSample = sample.GpuSample;
                for (const SceneGpuPassTiming& pass : sample.GpuPassTimings)
                    {
                        passCsv << sample.GpuSample << ",\"";
                        for (const char character : pass.Name)
                        {
                            if (character == '\"') passCsv << '\"';
                            passCsv << character;
                        }
                        passCsv << "\"," << pass.Milliseconds << ','
                            << sample.GraphSubmissionId << '\n';
                    }
            }
            if (sample.WallMilliseconds > 1000.0 / 60.0)
            {
                ++overBudget60;
                longestOverBudget60 = std::max(longestOverBudget60, ++consecutiveOverBudget60);
            }
            else consecutiveOverBudget60 = 0;
            if (sample.WallMilliseconds > 1000.0 / 90.0) ++overBudget90;
            if (++writtenFrames % 256u == 0) { csv.flush(); passCsv.flush(); wallSamples.flush(); }
        };
        CpuExecutionTimes previousCpuExecution;
        std::chrono::steady_clock::time_point previousCpuFrameStart;
		InputActions::Get().ReloadProjectMap();
		while (bRunning)
		{
            if (!mWindow->WaitForFrame()) break;
            if (mWindow->ConsumeResume())
            {
                simulationClock.DiscardSuspendedTime();
                presentationClock.DiscardSuspendedTime();
            }
			const auto measuredFrameStart = std::chrono::steady_clock::now();
            const auto phaseStart = [&]() {
                return mRunOptions.bRenderStats ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
            };
            const auto phaseMilliseconds = [&](std::chrono::steady_clock::time_point start) {
                return mRunOptions.bRenderStats ? std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count() : 0.0;
            };
            const auto statsPrologueStart = phaseStart();
            double statsQueryMilliseconds = 0.0;
            BeginAnimationCpuTiming(mRunOptions.bRenderStats);
            const double cpuStartUtcMilliseconds = mRunOptions.bRenderStats
                ? std::chrono::duration<double, std::milli>(
                    std::chrono::system_clock::now().time_since_epoch()).count() : 0.0;
            if (mRunOptions.bRenderStats)
            {
                const auto queryStart = phaseStart();
                const CpuExecutionTimes currentCpuExecution = QueryCpuExecutionTimes();
                statsQueryMilliseconds = phaseMilliseconds(queryStart);
                if (previousPerformanceFrame)
                {
                    auto& pending = *previousPerformanceFrame;
                    pending.CpuStartIntervalMilliseconds = std::chrono::duration<double, std::milli>(
                        measuredFrameStart - previousCpuFrameStart).count();
                    pending.bCpuIntervalValid = true;
                    if (currentCpuExecution.bValid && previousCpuExecution.bValid &&
                        currentCpuExecution.ThreadId == previousCpuExecution.ThreadId &&
                        currentCpuExecution.Thread100Nanoseconds >= previousCpuExecution.Thread100Nanoseconds &&
                        currentCpuExecution.Process100Nanoseconds >= previousCpuExecution.Process100Nanoseconds)
                    {
                        pending.CpuThreadBusyMilliseconds = double(currentCpuExecution.Thread100Nanoseconds -
                            previousCpuExecution.Thread100Nanoseconds) / 10000.0;
                        pending.CpuProcessBusyMilliseconds = double(currentCpuExecution.Process100Nanoseconds -
                            previousCpuExecution.Process100Nanoseconds) / 10000.0;
                        pending.bCpuBusyValid = true;
                    }
                }
                previousPerformanceFrame = nullptr;
                previousCpuFrameStart = measuredFrameStart;
                previousCpuExecution = currentCpuExecution;
            }
            const double statsPrologueMilliseconds = phaseMilliseconds(statsPrologueStart);
            ++cpuTimingFrameId;
            GpuTimingFrameGuard gpuTimingGuard{ mRunOptions.bRenderStats ? mRuntimeRenderer.get() : nullptr };
            const auto timingBeginStart = phaseStart();
            if (mRunOptions.bRenderStats) mRuntimeRenderer->BeginGpuTimingFrame(cpuTimingFrameId);
            const double timingBeginMilliseconds = phaseMilliseconds(timingBeginStart);
			double updateMilliseconds = 0.0;
			double renderSubmitMilliseconds = 0.0;
			double imGuiMilliseconds = 0.0;
			double runtimeRenderMilliseconds = 0.0;
			double windowMilliseconds = 0.0;
			double preUpdateMilliseconds = 0.0;
			RenderFrameCpuTimings backendCpuTimings;
			HE_PROFILE_FRAME("HEngine Main Frame");
			HE_PROFILE_SECTION("Application::Frame");

			const auto resourcePumpStart = phaseStart();
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
            const double resourcePumpMilliseconds = phaseMilliseconds(resourcePumpStart);

			{
				const auto preUpdateStart = std::chrono::steady_clock::now();
				HE_PROFILE_SECTION("Layer Pre-Updates");
				for (Layer* layer : mLayerStack)
					layer->OnPreUpdate();
				preUpdateMilliseconds = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - preUpdateStart).count();
			}
			if (!bRunning)
				break;

			const bool bAutomationFrame = !bRuntimeStartupPending;
			const auto audioInputStart = phaseStart();
			const Timestep presentationTimestep = presentationClock.BeginFrame(
				mRunOptions.FixedDeltaSeconds,
				bAutomationFrame && !bMinimized);
			const Timestep timestep = simulationClock.BeginFrame(
				mRunOptions.FixedDeltaSeconds,
				bAutomationFrame);
			AudioEngine::GetInstance().Update();
            const auto& inputRegion = ScriptInputContext::Region;
			InputActions::Get().UpdateFromDevices(presentationTimestep.GetSeconds(),
                !bMinimized && inputRegion.Enabled && inputRegion.KeyboardEnabled);
            const double audioInputMilliseconds = phaseMilliseconds(audioInputStart);
			HE_PROFILE_PLOT("Frame Time (ms)", timestep.GetMilliseconds());
			HE_PROFILE_PLOT("Presentation Frame Time (ms)", presentationTimestep.GetMilliseconds());
			HE_PROFILE_PLOT("FPS", timestep.GetSeconds() > 0.0f ? 1.0f / timestep.GetSeconds() : 0.0f);

            const bool bFrameRendered = !bMinimized;
			if (bFrameRendered)
			{
				BeginRenderFrameCpuTiming(mRunOptions.bRenderStats);
				mRuntimeRenderer->BeginSimulationFrame();
				struct SimulationFrameScope
				{
					RuntimeRenderer* Renderer;
					~SimulationFrameScope() { Renderer->AbortSimulationFrame(); }
				} simulationFrameScope{mRuntimeRenderer.get()};
				const auto updateStart = std::chrono::steady_clock::now();
				static const bool bTimeLayerUpdates = [] {
					const char* setting = std::getenv("HE_LAYER_UPDATE_TIMINGS");
					return setting && setting[0] == '1' && setting[1] == '\0';
				}();
				std::array<std::pair<Layer*, double>, 16> layerTimings{};
				size_t timedLayerCount = 0;
				{
					HE_PROFILE_SECTION("Layer Updates");
					for (Layer* layer : mLayerStack)
					{
						HE_PROFILE_TRANSIENT_SECTION(layer->GetName().c_str());
						const auto layerStart = bTimeLayerUpdates ? std::chrono::steady_clock::now()
							: std::chrono::steady_clock::time_point{};
						layer->OnUpdate(timestep);
						if (bTimeLayerUpdates && timedLayerCount < layerTimings.size())
							layerTimings[timedLayerCount++] = { layer,
								std::chrono::duration<double, std::milli>(
									std::chrono::steady_clock::now() - layerStart).count() };
					}
				}

				const bool bCaptureThisFrame = bAutomationFrame &&
					mRunOptions.HasFrameCapture() &&
					simulationClock.GetFrameNumber() == mRunOptions.CaptureFrame;
				const bool bRuntimeCaptureThisFrame =
					mFrameCaptureProgress.State == RuntimeFrameCaptureState::Queued &&
                        mSequenceSettleRemaining == 0;
                const bool bSequenceHistoryFrame = bSequenceCameraCut && !mSequenceCaptures.empty() &&
                    mFrameCaptureProgress.State == RuntimeFrameCaptureState::Queued;
				if (bCaptureThisFrame && bRuntimeCaptureThisFrame)
					throw std::runtime_error("Automation and runtime captures requested the same frame");
				RuntimeUiOverlayRecorder overlayRecorder(
					*mNvrhiImGuiLayer,
					RmlUiManager::GetInstance());
				RuntimeRendererFrameResult frameResult;
				const auto renderStart = std::chrono::steady_clock::now();
				updateMilliseconds = std::chrono::duration<double, std::milli>(
					renderStart - updateStart).count();
				if (bTimeLayerUpdates && updateMilliseconds > 20.0)
				{
					std::ostringstream details;
					double layerSum = 0.0;
					for (size_t index = 0; index < timedLayerCount; ++index)
					{
						layerSum += layerTimings[index].second;
						details << ' ' << layerTimings[index].first->GetName()
							<< '=' << layerTimings[index].second;
					}
					HE_CORE_WARN("[ApplicationLayerTiming] frame={} updateMs={} layersMs={} outsideLayersMs={} details:{}",
						simulationClock.GetFrameNumber(), updateMilliseconds, layerSum,
						updateMilliseconds - layerSum, details.str());
				}
				if (mUiTextureRegistry)
				{
					for (auto& [key, registration] : mUiTextureRegistry->Registrations)
					{
						const ManagedTexture& resource = mRuntimeRenderer->GetTexture(registration.Managed);
						if (resource.Texture == registration.BoundTexture)
							continue;
						const ImGuiTextureHandle handle{
							.Index = static_cast<uint32_t>(key),
							.Generation = static_cast<uint32_t>(key >> 32u),
						};
						mNvrhiImGuiLayer->ReplaceTexture(handle, resource.Texture,
							nvrhi::Format::UNKNOWN, mLastFrameSlot);
						registration.BoundTexture = resource.Texture;
					}
				}
				mNvrhiImGuiLayer->BeginCpuFrame();
				try
				{
					const auto imGuiStart = phaseStart();
					{
						HE_PROFILE_SECTION("ImGui Frame");
						for (Layer* layer : mLayerStack)
						{
							HE_PROFILE_TRANSIENT_SECTION(layer->GetName().c_str());
							layer->OnImGuiRender();
						}
					}
					imGuiMilliseconds = phaseMilliseconds(imGuiStart);

					const auto runtimeRenderStart = phaseStart();
					frameResult = mRuntimeRenderer->RenderFrame(
						RuntimeRendererFrameDesc{
							.World = mRenderWorld,
							.OverlayRecorder = &overlayRecorder,
							.CapturePath = bCaptureThisFrame
								? mRunOptions.CapturePath
								: (bRuntimeCaptureThisFrame
									? mFrameCaptureProgress.OutputPath
									: std::filesystem::path{}),
                            .CaptureSource = bRuntimeCaptureThisFrame
                                ? mFrameCaptureProgress.Source : FrameCaptureSource::Automatic,
                            .ApplicationSimulationFrame = simulationClock.GetFrameNumber(),
                            .bApplicationSimulationFrameValid = bCaptureThisFrame || bRuntimeCaptureThisFrame,
                            .bCaptureSequenceHistory = bSequenceHistoryFrame,
						},
						mWindow->IsVSync());
					runtimeRenderMilliseconds = phaseMilliseconds(runtimeRenderStart);
				}
				catch (...)
				{
                    if (bSequenceHistoryFrame) CancelFrameCaptureSequence();
					(void)EndRenderFrameCpuTiming();
					mNvrhiImGuiLayer->CancelCpuFrame();
					throw;
				}

                if (bSequenceHistoryFrame)
                {
                    Json record = std::move(frameResult.SequenceHistory);
                    record["applicationSimulationFrame"] = simulationClock.GetFrameNumber();
                    record["simulationStep"] = mCaptureSimulationStep;
                    record["phase"] = mSequenceSettleRemaining ? "settle" : "capture";
                    const bool first = mSequenceHistory.empty();
                    const bool valid = record.value("valid", false) && frameResult.Submission != 0 &&
                        (first ? record.value("cameraCut", false) && record.value("historyReset", false)
                               : !record.value("historyReset", true) &&
                                 record.value("previousSubmissionId", uint64_t(0)) == mSequenceHistory.back().value("submission", uint64_t(0))) &&
                        (first || (record.at("settingsHash") == mSequenceHistory.front().at("settingsHash") &&
                                   record.at("sceneGeneration") == mSequenceHistory.front().at("sceneGeneration") &&
                                   record.at("giTechnique") == mSequenceHistory.front().at("giTechnique"))) &&
                        (record.value("giTechnique", 0u) == 0u ||
                            (record.value("giFrameSequenceIndex", uint64_t(-1)) == mSequenceHistory.size() &&
                             (first || record.at("giHistoryResetCount") == mSequenceHistory.front().at("giHistoryResetCount")))) &&
                        (!mSequenceSettleRemaining || (mCaptureSimulationStep == mSequenceSettleSimulationStep &&
                            (first || record.at("viewColumnMajor") == mSequenceHistory.front().at("viewColumnMajor"))));
                    record["contractValid"] = valid;
                    record["settleRemainingBefore"] = mSequenceSettleRemaining;
                    mSequenceHistory.push_back(std::move(record));
                    if (!valid)
                    {
                        mFrameCaptureProgress.Error = "Atomic sequence history/settle submission contract failed";
                        if (frameResult.Capture && mRuntimeRenderer->IsCaptureValid(frameResult.Capture))
                            mRuntimeRenderer->ReleaseCapture(frameResult.Capture);
                        CancelFrameCaptureSequence();
                    }
                    else if (mSequenceSettleRemaining) --mSequenceSettleRemaining;
                }

				if (bCaptureThisFrame)
				{
					if (!frameResult.Capture)
						throw std::runtime_error("The RenderGraph capture pass did not queue a readback");
					automationCapture = frameResult.Capture;
				}
                else if (bRuntimeCaptureThisFrame && mFrameCaptureProgress.State == RuntimeFrameCaptureState::Failed)
                {
                    // Cancellation already retired this frame readback above.
                }
                else if (bRuntimeCaptureThisFrame && !frameResult.CaptureError.empty())
                {
                    // Only a known absent native source is recoverable. Normal
                    // rendering/submission completed; arbitrary exceptions retain
                    // the existing fatal/rollback path.
                    mFrameCaptureProgress.State = RuntimeFrameCaptureState::Failed;
                    mFrameCaptureProgress.Error = frameResult.CaptureError;
                }
				else if (bRuntimeCaptureThisFrame)
				{
					if (!frameResult.Capture)
						throw std::runtime_error("The runtime capture pass did not queue a readback");
					mFrameCaptureHandle = frameResult.Capture;
					mFrameCaptureProgress.State = RuntimeFrameCaptureState::InFlight;
                    // Save recording identity before asynchronous readback completion.
                    mFrameCaptureProgress.RenderFrame = simulationClock.GetFrameNumber();
                    mFrameCaptureProgress.Submission = frameResult.Submission;
                    mFrameCaptureProgress.RenderWorldFrame = mRenderWorld ? mRenderWorld->FrameNumber : 0;
                    if (!mSequenceCaptures.empty())
                    {
                        auto& entry = mSequenceCaptures.at(mSequenceRecorded);
                        entry.State = RuntimeFrameCaptureState::InFlight;
                        entry.RenderFrame = simulationClock.GetFrameNumber();
                        entry.Submission = frameResult.Submission;
                        entry.RenderWorldFrame = mFrameCaptureProgress.RenderWorldFrame;
                        entry.SimulationTime = mRenderWorld ? mRenderWorld->TimeSeconds : 0.0;
                        entry.SimulationStep = mCaptureSimulationStep;
                        mSequenceHandles[mSequenceRecorded++] = mFrameCaptureHandle;
                        mFrameCaptureHandle = {};
                        if (mSequenceRecorded < mSequenceCaptures.size())
                        {
                            mFrameCaptureProgress.State = RuntimeFrameCaptureState::Queued;
                            mFrameCaptureProgress.OutputPath = mSequenceCaptures[mSequenceRecorded].OutputPath;
                        }
                    }
				}
				mLastFrameSlot = frameResult.FrameSlot;
				mLastSubmission = frameResult.Submission;
				renderSubmitMilliseconds = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - renderStart).count();
				backendCpuTimings = EndRenderFrameCpuTiming();

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
				const auto windowStart = std::chrono::steady_clock::now();
				mWindow->OnUpdate();
				windowMilliseconds = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - windowStart).count();
			}

			const auto capturePollStart = phaseStart();
            simulationClock.CompleteFrame();
			presentationClock.CompleteFrame();
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
            const double capturePollMilliseconds = phaseMilliseconds(capturePollStart);
            const auto submissionCollectStart = phaseStart();
            if (mRunOptions.bRenderStats) collectSubmissionTimings();
            const double submissionCollectMilliseconds = phaseMilliseconds(submissionCollectStart);
            const auto memorySampleNow = phaseStart();
            if (mRunOptions.bRenderStats && bFrameRendered &&
                (!bHasMemorySample || memorySampleNow - lastMemorySample >= std::chrono::seconds(1)))
            {
                const auto budget = mRuntimeRenderer->QueryMemoryBudget();
                const double queryMilliseconds = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - memorySampleNow).count();
                memorySamples.Append({completedFrameCount,
                    std::chrono::duration<double>(memorySampleNow - memorySampleStart).count(),
                    queryMilliseconds, budget, bAutomationFrame});
                bHasMemorySample = true;
                lastMemorySample = memorySampleNow;
            }
            const double memorySampleMilliseconds = phaseMilliseconds(memorySampleNow);
			const bool bReachedFrameLimit = bAutomationFrame && mRunOptions.ExitAfterFrames > 0 &&
				completedFrameCount >= mRunOptions.ExitAfterFrames;
			if (bReachedFrameLimit)
			{
				bRunning = false;
			}
			if (mRunOptions.bRenderStats && bAutomationFrame && bFrameRendered &&
				completedFrameCount > mRunOptions.PerformanceWarmupFrames)
			{
				++measuredFrameCount;
				const SceneRendererStats& sceneStats = mRuntimeRenderer->GetSceneStats();
				if (measuredFrameCount == 1)
					gpuWarmupCutoffSample = sceneStats.GpuRenderGraphSampleCount +
						mRuntimeRenderer->GetFramesInFlight();
				pendingPerformanceFrame = PerformanceFrame{ completedFrameCount, 0.0,
					updateMilliseconds, renderSubmitMilliseconds, windowMilliseconds,
					mRuntimeRenderer->GetWidth(), mRuntimeRenderer->GetHeight(),
                    sceneStats.GpuRenderGraphSampleCount,
                    mRuntimeRenderer->GetLastTimingSampleId(), cpuTimingFrameId,
                    sceneStats.GpuRenderGraphLastMilliseconds,
					sceneStats.GpuFrameSpanMilliseconds, sceneStats.SceneTableUploadedBytes,
					sceneStats.GpuDrivenObjectCount, sceneStats.GpuVisibleObjectCount,
					sceneStats.GpuDrivenBatchCount, sceneStats.DirectionalShadowDrawCount,
					sceneStats.RenderGraphTransientReservedBytes, backendCpuTimings,
                    sceneStats.GpuPassTimings, sceneStats.GpuVisibleTriangleCount,
                    sceneStats.GpuCullingReadbackSubmission, sceneStats.GpuCullingReadbackInputObjects,
                    sceneStats.GpuCullingReadbackBatches,
                    sceneStats.BaseRasterTriangleCount, sceneStats.SelectedRasterTriangleCount,
                    sceneStats.ReducedLodSectionCount,
                    sceneStats.GeometryArenaReservedBytes, sceneStats.GeometryArenaUsedBytes,
                    sceneStats.IndirectCompactionResidentBytes, sceneStats.IndirectCompactionUploadedBytes,
                    sceneStats.GpuMultiDrawGroupCount,
                    sceneStats.GpuShadowIndirectDrawCount, sceneStats.GpuShadowMultiDrawGroupCount,
                    sceneStats.GpuShadowRecordedViews, sceneStats.GpuShadowCompactedViews,
                    sceneStats.GpuShadowResidentBytes, sceneStats.GpuShadowUploadedBytes,
                    preUpdateMilliseconds, sceneStats.CloudCpuRecordMilliseconds,
                    sceneStats.CpuFramePreparationMilliseconds, sceneStats.CpuMaterialPackingMilliseconds,
                    sceneStats.MaterialPackingCallCount, sceneStats.MaterialPackingCacheHitCount,
                    sceneStats.bMaterialPackingCacheEnabled, sceneStats.RenderGraphCachedTransientBytes,
                    sceneStats.RenderGraphRetiredResourceBytes, sceneStats.RenderGraphTrimmedTransientBytes,
                    sceneStats.CustomMaterialGpuDrawCount, sceneStats.CustomMaterialGpuObjectCount,
                    sceneStats.CpuGraphRecordMilliseconds, sceneStats.CpuGraphBuildMilliseconds,
                    sceneStats.GraphBuildReasonMask };
                pendingPerformanceFrame->bRuntimeSceneLoading = IsRuntimeSceneLoading();
                pendingPerformanceFrame->PendingTextureUploads = sceneStats.PendingTextureUploads;
                pendingPerformanceFrame->InternalWidth = sceneStats.InternalWidth;
                pendingPerformanceFrame->InternalHeight = sceneStats.InternalHeight;
                pendingPerformanceFrame->GraphSubmissionId = sceneStats.GpuRenderGraphLastSubmissionId;
                pendingPerformanceFrame->bCpuCounterFrameValid = sceneStats.bCpuCounterFrameValid;
                pendingPerformanceFrame->CpuCounterWorldFrame = sceneStats.CpuCounterWorldFrame;
                pendingPerformanceFrame->InputRenderObjects = sceneStats.InputRenderObjects;
                pendingPerformanceFrame->PreparedRasterSections = sceneStats.PreparedRasterSections;
                pendingPerformanceFrame->SectionsWithAuthoredLods = sceneStats.SectionsWithAuthoredLods;
                pendingPerformanceFrame->SelectedLodSections = sceneStats.SelectedLodSections;
                pendingPerformanceFrame->DirectionalCpuCasterDraws = sceneStats.DirectionalCpuCasterDraws;
                pendingPerformanceFrame->LocalShadowActiveViews = sceneStats.LocalShadowActiveViews;
                pendingPerformanceFrame->LocalShadowDirtyViews = sceneStats.LocalShadowDirtyViews;
                pendingPerformanceFrame->LocalShadowCachedViews = sceneStats.LocalShadowCachedViews;
                pendingPerformanceFrame->LocalShadowVisibleCasters = sceneStats.LocalShadowVisibleCasters;
                pendingPerformanceFrame->GiBudget = sceneStats.GiStats;
                pendingPerformanceFrame->AnimationCpu = sAnimationCpuTimings;
                pendingPerformanceFrame->CpuStatsPrologueMilliseconds = statsPrologueMilliseconds;
                pendingPerformanceFrame->CpuTimingBeginMilliseconds = timingBeginMilliseconds;
                pendingPerformanceFrame->CpuResourcePumpMilliseconds = resourcePumpMilliseconds;
                pendingPerformanceFrame->CpuAudioInputMilliseconds = audioInputMilliseconds;
                pendingPerformanceFrame->CpuCapturePollMilliseconds = capturePollMilliseconds;
                pendingPerformanceFrame->CpuSubmissionCollectMilliseconds = submissionCollectMilliseconds;
                pendingPerformanceFrame->CpuMemorySampleMilliseconds = memorySampleMilliseconds;
                pendingPerformanceFrame->CpuStatsQueryMilliseconds = statsQueryMilliseconds;
                pendingPerformanceFrame->CpuImGuiMilliseconds = imGuiMilliseconds;
                pendingPerformanceFrame->CpuRuntimeRenderMilliseconds = runtimeRenderMilliseconds;
                // Current CPU identity, never a delayed GPU sample. Names are interned once.
                // The bounded POD append cost is part of the diagnostic workload.
                if (sceneStats.CpuGraphTimingCount > cpuGraphRowLimit - cpuGraphRows.size())
                    throw std::runtime_error("CPU graph statistics buffer exhausted; no rows were silently dropped.");
                for (uint32_t phaseIndex = 0; phaseIndex < sceneStats.CpuGraphTimingCount; ++phaseIndex)
                {
                    const auto& phase = sceneStats.CpuGraphTimings[phaseIndex];
                    auto found = cpuGraphNameIds.find(phase.Name);
                    if (found == cpuGraphNameIds.end())
                    {
                        const auto id = static_cast<uint32_t>(cpuGraphNames.size());
                        cpuGraphNames.push_back(phase.Name);
                        found = cpuGraphNameIds.emplace(phase.Name, id).first;
                    }
                    cpuGraphRows.push_back({pendingPerformanceFrame->CpuTimingFrameId,
                        sceneStats.CpuCounterWorldFrame, sceneStats.CpuGraphSubmissionId,
                        phase.Milliseconds, found->second, phase.ExecutionIndex, phase.PhaseIndex,
                        phase.bCompleted && sceneStats.bCpuGraphTimingValid});
                }

                pendingPerformanceFrame->CpuThreadId = previousCpuExecution.ThreadId;
                pendingPerformanceFrame->CpuStartUtcMilliseconds = cpuStartUtcMilliseconds;
                // Complete normal guard cleanup before the measurement endpoint. On an
                // exception the still-armed guard retains the existing cleanup behavior.
                if (gpuTimingGuard.Renderer)
                {
                    gpuTimingGuard.Renderer->CancelGpuTimingFrame();
                    gpuTimingGuard.Renderer = nullptr;
                }
                // Reserve prevents relocation. Append belongs to this frame's v2
                // wall interval, including the final frame with no next iteration.
                const auto bufferStart = phaseStart();
                performanceFrames.Append(std::move(*pendingPerformanceFrame));
                pendingPerformanceFrame.reset();
                previousPerformanceFrame = &performanceFrames.Back();
                previousPerformanceFrame->CpuStatsBufferMilliseconds = phaseMilliseconds(bufferStart);
                const double seconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - measuredFrameStart).count();
                previousPerformanceFrame->WallMilliseconds = seconds * 1000.0;
                measuredFrameSeconds += seconds;
			}
			if (bReachedFrameLimit)
				HE_CORE_INFO("[Automation] Completed frame limit: {0}", completedFrameCount);
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
		if (mWindow && mRuntimeRenderer && mRuntimeRenderer->IsInitialized())
		{
			try { WriteRenderingSettingsEvidence(mRunOptions, *mRuntimeRenderer, *mWindow, "shutdown"); }
			catch (const std::exception& exception)
			{ HE_CORE_ERROR("Rendering settings evidence write failed: {}", exception.what()); }
		}

		if (mRunOptions.HasFrameCapture() && !bCaptureCompleted)
			throw std::runtime_error("The application closed before the requested automation frame was captured.");
		if (mRunOptions.bRenderStats)
		{
			// GPU timestamps are resolved only after their owning submissions have
			// retired. This is the one intentional synchronization point for stats.
			mRuntimeRenderer->WaitForIdle();
            collectSubmissionTimings();
            // The final frame was buffered before its wall endpoint. Without a next
            // CPU frame, its interval/busy fields retain explicit unavailable values.
            // Formatting, filesystem writes and flushes occur only after capture.
            // Ordered buffers retain all measured records, including the final frame.
            for (const auto& sample : performanceFrames.Rows()) writePerformanceFrame(sample);
            writeSubmissionTimings();
            for (const auto& sample : memorySamples.Rows()) writeMemorySample(sample);
            HE_CORE_INFO("[RenderStatistics] Structured capture frames={}/{}, submissions={}/{}, memory={}/{}",
                performanceFrames.Size(), performanceFrames.Limit(), submissionSamples.Size(), submissionSamples.Limit(),
                memorySamples.Size(), memorySamples.Limit());
            HE_CORE_INFO("[RenderStatistics] CPU graph buffered rows={}, used POD bytes={}, reserved POD bytes={}, unique names={}",
                cpuGraphRows.size(), cpuGraphRows.size() * sizeof(CpuGraphCsvRow),
                cpuGraphRows.capacity() * sizeof(CpuGraphCsvRow), cpuGraphNames.size());
            static constexpr const char* cpuPhaseLabels[] = {
                "Compile", "Realize", "Validate", "Setup", "Barriers", "Record", "Trailing"};
            for (const auto& row : cpuGraphRows)
            {
                cpuPassCsv << row.CpuFrame << ',' << row.WorldFrame << ',' << row.Submission << ','
                    << (row.Submission != 0 ? 1 : 0) << ',' << cpuPhaseLabels[row.Phase] << ','
                    << row.ExecutionIndex << ",\"";
                for (const char character : cpuGraphNames[row.NameIndex])
                {
                    if (character == '\"') cpuPassCsv << '\"';
                    cpuPassCsv << character;
                }
                cpuPassCsv << "\"," << row.Milliseconds << ',' << (row.bCompleted ? 1 : 0) << '\n';
            }
            csv.flush(); submissionCsv.flush(); passCsv.flush(); cpuPassCsv.flush(); memoryCsv.flush(); wallSamples.flush();
            if (writtenFrames != measuredFrameCount) throw std::runtime_error("Render statistics frame count mismatch");
            csv.close(); submissionCsv.close(); passCsv.close(); cpuPassCsv.close(); memoryCsv.close(); wallSamples.close();
            HE_CORE_INFO("[RenderStatistics] Completed frames={0}; submissionRows={1}; pendingFrameCapacity=1",
                writtenFrames, writtenSubmissions);
			const double averageMilliseconds = measuredFrameCount > 0
				? measuredFrameSeconds * 1000.0 / static_cast<double>(measuredFrameCount)
				: 0.0;
			const double framesPerSecond = measuredFrameSeconds > 0.0
				? static_cast<double>(measuredFrameCount) / measuredFrameSeconds
				: 0.0;
			const std::string sceneStats = mRuntimeRenderer->BuildSceneStatsSummary();
            // All live-frame history is on disk. Only shutdown allocates this compact column.
            if (writtenFrames > std::numeric_limits<size_t>::max() / sizeof(double) ||
                std::filesystem::file_size(wallSamplesPath) != writtenFrames * sizeof(double))
                throw std::runtime_error("Render statistics exact wall-time spool is incomplete");
            std::vector<double> sortedFrameMilliseconds(static_cast<size_t>(writtenFrames));
            std::ifstream wallInput(wallSamplesPath, std::ios::binary);
            wallInput.exceptions(std::ios::badbit | std::ios::failbit);
            if (writtenFrames)
                wallInput.read(reinterpret_cast<char*>(sortedFrameMilliseconds.data()),
                    static_cast<std::streamsize>(writtenFrames * sizeof(double)));
			std::sort(sortedFrameMilliseconds.begin(), sortedFrameMilliseconds.end());
			const auto percentile = [&sortedFrameMilliseconds](double p)
			{
				if (sortedFrameMilliseconds.empty())
					return 0.0;
				const double position = p * static_cast<double>(sortedFrameMilliseconds.size() - 1);
				const size_t lower = static_cast<size_t>(position);
				const size_t upper = std::min(lower + 1, sortedFrameMilliseconds.size() - 1);
				return std::lerp(sortedFrameMilliseconds[lower], sortedFrameMilliseconds[upper],
					position - static_cast<double>(lower));
			};
			std::fprintf(
				stdout,
				"[RenderPerformance] samples=%llu avgMs=%.3f cpuWallAvgMs=%.3f fps=%.1f "
				"p95Ms=%.3f p99Ms=%.3f maxMs=%.3f overBudget60=%llu overBudget90=%llu "
				"longestOverBudget60=%llu warmupFrames=%llu width=%u height=%u vsync=%u validation=%u %s\n",
				static_cast<unsigned long long>(measuredFrameCount),
				averageMilliseconds,
				averageMilliseconds,
				framesPerSecond,
				percentile(0.95), percentile(0.99), percentile(1.0),
				static_cast<unsigned long long>(overBudget60),
				static_cast<unsigned long long>(overBudget90),
				static_cast<unsigned long long>(longestOverBudget60),
				static_cast<unsigned long long>(mRunOptions.PerformanceWarmupFrames),
				mRuntimeRenderer->GetWidth(), mRuntimeRenderer->GetHeight(),
				mWindow->IsVSync() ? 1u : 0u,
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
			mRuntimeRenderer->AbortSimulationFrame();
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
        CancelFrameCaptureSequence();
        mSequenceCaptures.clear();
        mSequenceHandles.clear();
        mSequenceRecorded = 0;
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
#if defined(HE_DEV_AGENT_RUNTIME)
			if (!mTextureStreamingProbe.empty())
				(void)InspectTextureStreamingProbe(true);
#endif
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
