#include "RuntimeGameLayer.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/Profile/Profile.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/RuntimeProjectResourceWarmup.h"
#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceCatalog.h"
#include "Runtime/EcsFramework/Serializer/SceneSerializer.h"
#include "Runtime/EcsFramework/Component/Light/EnvironmentLightComponent.h"
#include "Runtime/EcsFramework/System/Render/RenderSystem2D.h"
#include "Runtime/EcsFramework/System/Render/RenderSystem3D.h"
#include "Runtime/EcsFramework/System/Script/ScriptSystem.h"
#include "Runtime/Graphics/Renderer/RuntimeRenderer.h"
#include "Runtime/Graphics/Renderer/RenderPipeline.h"
#include "Runtime/Graphics/Renderer2D.h"
#include "Runtime/EcsFramework/System/UI/UIViewportContext.h"
#include "Runtime/Scripting/ScriptInputContext.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Core/Input/Input.h"
#include "Runtime/Core/Input/KeyCodes.h"
#include "Runtime/Core/SimulationClock.h"
#include "Runtime/UI/Rml/RmlUiManager.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] std::string FindEnvironmentPath(Level& level)
        {
            auto environmentView = level.mRegistry.view<EnvironmentLightComponent>();
            for (const auto entity : environmentView)
                return environmentView.get<EnvironmentLightComponent>(entity).HdrTexturePath;
            return {};
        }

		[[nodiscard]] Ref<const RenderWorld> AcquireMergedRenderWorld(Level& level)
		{
			Ref<const RenderWorld> world3D;
			Ref<const RenderWorld> world2D;
			if (RenderSystem3D* renderSystem = level.TryGetSystem<RenderSystem3D>())
				world3D = renderSystem->AcquireLatestRenderWorld();
			if (RenderSystem2D* renderSystem = level.TryGetSystem<RenderSystem2D>())
				world2D = renderSystem->AcquireLatestRenderWorld();
			if (!world3D)
				return world2D;
			if (!world2D || world2D->Primitives2D.empty())
				return world3D;

			Ref<RenderWorld> merged = CreateRef<RenderWorld>(*world3D);
			merged->Primitives2D.insert(
				merged->Primitives2D.end(),
				world2D->Primitives2D.begin(),
				world2D->Primitives2D.end());
			return merged;
		}

        void ReportCleanupFailure(
            std::string_view operation,
            const std::exception_ptr& failure) noexcept
        {
            try
            {
                if (failure)
                    std::rethrow_exception(failure);
            }
            catch (const std::exception& error)
            {
                Log::WriteFallbackError(operation, error.what());
            }
            catch (...)
            {
                Log::WriteFallbackError(operation, "unknown exception");
            }
        }
    }

    RuntimeGameLayer::RuntimeGameLayer()
    {
        HE_CORE_INFO("Game Begin!");
    }

    void RuntimeGameLayer::RefreshLoadingScreenSettings()
    {
        // Optional project presentation settings. Saved preference files use the
        // same overlay convention as the public settings.read script service.
        mCopperLoadingScreen = false;
        mLoadingScreenChinese = false;
        mLoadingScreenTitle.clear();
        try
        {
            const auto& config = ConfigManager::GetInstance();
            const auto gameConfigPath = config.GetGameConfigPath();
            const Json document = Utils::LoadJsonFile(gameConfigPath.is_absolute()
                ? gameConfigPath : config.GetRootFolder() / gameConfigPath,
                { 1024 * 1024, 32, 65536 }).Value;
            if (!document.contains("Game") || !document["Game"].contains("LoadingScreen"))
                return;
            const auto& settings = document["Game"]["LoadingScreen"];
            mLoadingScreenDocumentPath = settings.value("DocumentPath", std::string("Assets/UI/Loading/CopperNoir.rml"));
            mCopperLoadingScreen = settings.value("Style", std::string("Classic")) == "CopperNoir";
            mLoadingScreenTitle = settings.value("Title", std::string{});
            std::string language = settings.value("Language", std::string("zh-CN"));
            const auto preferencePath = settings.value("LanguagePreferencePath", std::string{});
            if (!preferencePath.empty())
            {
                std::string normalized;
                const auto valid = Utils::TryNormalizeProjectRelativePath(preferencePath, normalized);
                if (!valid || !normalized.starts_with("Config/") || !normalized.ends_with(".json"))
                    throw std::invalid_argument("Game.LoadingScreen.LanguagePreferencePath requires a Config-relative JSON path.");
                for (const auto& path : { ConfigManager::GetProjectFullPath(normalized), ConfigManager::GetSavedPath(normalized) })
                {
                    if (!std::filesystem::exists(path))
                        continue;
                    const auto preferences = Utils::LoadJsonFile(path, { 65536, 16, 4096 }).Value;
                    if (preferences.is_object() && preferences.contains("Language") && preferences["Language"].is_string())
                        language = preferences["Language"].get<std::string>();
                }
            }
            mLoadingScreenChinese = language.starts_with("zh");
        }
        catch (const std::exception& error)
        {
            HE_CORE_WARN("[Game] Loading screen configuration: {0}", error.what());
        }
    }

    void RuntimeGameLayer::OnAttach()
    {
        HE_PROFILE_SECTION("RuntimeGameLayer::OnAttach");
        RefreshLoadingScreenSettings();

        if (auto* imguiLayer = Application::GetInstance().GetImGuiLayer())
            imguiLayer->BlockEvents(false);

        Application::GetWindow().SetInputMethodEnabled(false);

        mRuntimeAltDown = false;
        SetRuntimeCursorCaptured(!Application::GetInstance().GetRunOptions().bNoCursorCapture);

        const auto initialWidth = std::max<std::uint32_t>(Application::GetWindow().GetWidth(), 1u);
        const auto initialHeight = std::max<std::uint32_t>(Application::GetWindow().GetHeight(), 1u);
        ConfigManager::mViewportSize = {
            static_cast<float>(initialWidth),
            static_cast<float>(initialHeight)
        };

        Application::GetInstance().BeginRuntimeStartup();
        mStartupState = StartupState::AwaitFirstPresent;
    }

    void RuntimeGameLayer::AdvanceStartup()
    {
        Application& application = Application::GetInstance();
        RuntimeResourceRegistry& registry = RuntimeResourceRegistry::GetInstance();

        switch (mStartupState)
        {
            case StartupState::AwaitFirstPresent:
            {
                if (!application.HasRuntimeStartupPresentedFrame())
                    break;
                mStartupState = StartupState::LoadGameConfig;
            }
            [[fallthrough]];
            case StartupState::LoadGameConfig:
            {
                HE_PROFILE_SECTION("RuntimeGameLayer::LoadGameConfig");
                RuntimeProjectResourceWarmup& warmup = application.GetProjectResourceWarmup();
                const ApplicationRunOptions& runOptions = application.GetRunOptions();
                mDefaultScene = runOptions.StartupScene.empty()
                    ? warmup.GetDefaultScene()
                    : runOptions.StartupScene;
                mSceneDocumentPath = ResolveSceneDocumentPath(
                    ConfigManager::GetInstance().GetRootFolder(),
                    mDefaultScene);

                if (warmup.QueueGroup("Boot"))
                {
                    mStartupState = StartupState::WaitBootResources;
                }
                else
                {
                    // ResourceWarmup is optional for legacy loose projects and v1
                    // packages without a catalog. They retain the staged scene path.
                    mStartupState = StartupState::QueueSceneDocument;
                    break;
                }
            }
            [[fallthrough]];
            case StartupState::WaitBootResources:
            {
                RuntimeProjectResourceWarmup& warmup = application.GetProjectResourceWarmup();
                const auto progress = warmup.GetGroupProgress("Boot");
                if (!progress)
                    throw std::logic_error("Game startup lost its Boot resource warm-up group.");
                if (progress->State == RuntimeResourceWarmupGroupState::Failed)
                {
                    warmup.RethrowFailure("Boot");
                    throw std::runtime_error(
                        "Boot resource warm-up failed without reporting an exception.");
                }
                if (progress->State == RuntimeResourceWarmupGroupState::Idle ||
                    progress->State == RuntimeResourceWarmupGroupState::Released)
                {
                    throw std::logic_error(
                        "Boot resource warm-up left its queued startup state.");
                }
                if (progress->State != RuntimeResourceWarmupGroupState::Ready)
                    break;
                mStartupState = StartupState::QueueSceneDocument;
            }
            [[fallthrough]];
            case StartupState::QueueSceneDocument:
            {
                if (!registry.IsInitialized())
                    throw std::logic_error("Runtime resource registry is not initialized during game startup.");

                mSceneDocumentGroup = registry.CreateGroup();
                (void)QueueSceneDocument(
                    registry,
                    mSceneDocumentPath,
                    mSceneDocumentGroup);
                mStartupState = StartupState::WaitSceneDocument;
            }
            [[fallthrough]];
            case StartupState::WaitSceneDocument:
            {
                const RuntimeResourceState state = registry.GetState(mSceneDocumentPath.Key);
                if (state == RuntimeResourceState::Failed)
                {
                    (void)AcquireSceneDocument(registry, mSceneDocumentPath);
                    throw std::runtime_error(
                        "Scene document load failed without reporting an exception: " + mDefaultScene);
                }
                if (state == RuntimeResourceState::Unloaded ||
                    state == RuntimeResourceState::EvictPending)
                {
                    throw std::runtime_error(
                        "Scene document became unavailable during game startup: " + mDefaultScene);
                }
                if (state != RuntimeResourceState::Ready)
                    break;

                RuntimeResourceHandle<SceneDocument> handle =
                    registry.TryAcquireReady<SceneDocument>(mSceneDocumentPath.Key);
                if (!handle)
                    throw std::runtime_error("Ready scene document could not be acquired: " + mDefaultScene);
                std::shared_ptr<const SceneDocument> document = handle.GetShared();
                if (!document)
                    throw std::runtime_error("Ready scene document lease could not be created: " + mDefaultScene);

                mSceneDocumentGroup.Release();
                mSceneMaterializer.Start(
                    registry,
                    std::move(document),
                    mSceneDocumentPath.ProjectRoot,
                    RuntimeResourceCatalog::GetInstance().GetSnapshot());
                const SceneMaterializationProgress progress = mSceneMaterializer.GetProgress();
                if (progress.State == SceneMaterializationState::Failed ||
                    progress.State == SceneMaterializationState::Cancelled)
                {
                    mSceneMaterializer.RethrowFailure();
                    throw std::runtime_error("Scene materializer failed to start: " + mDefaultScene);
                }
                mStartupState = StartupState::MaterializeScene;
            }
            [[fallthrough]];
            case StartupState::MaterializeScene:
            {
                const SceneMaterializationProgress progress = mSceneMaterializer.Pump({
                    std::chrono::milliseconds(2),
                    16
                });
                if (progress.State == SceneMaterializationState::Failed ||
                    progress.State == SceneMaterializationState::Cancelled)
                {
                    mSceneMaterializer.RethrowFailure();
                    throw std::runtime_error("Scene materialization failed: " + mDefaultScene);
                }
                if (progress.State == SceneMaterializationState::Ready)
                {
                    mPendingScene = mSceneMaterializer.TakeCompletedLevel();
                    if (!mPendingScene)
                        throw std::runtime_error("Completed scene materialization produced no level.");
                    mStartupState = StartupState::PrepareRuntimeResources;
                }
                else if (progress.State == SceneMaterializationState::Empty)
                {
                    throw std::logic_error("Scene materializer lost its startup state.");
                }
                else
                {
                    break;
                }
            }
            [[fallthrough]];
            case StartupState::PrepareRuntimeResources:
            {
                if (!mPendingScene)
                    throw std::logic_error("Game startup has no scene for runtime resource preparation.");

                const std::string environmentPath = FindEnvironmentPath(*mPendingScene);
                const RuntimeEnvironmentPreparation preparation =
                    application.PrepareRenderEnvironment(environmentPath);
                if (preparation.State == RuntimeEnvironmentState::Failed)
                {
                    throw std::runtime_error(
                        "Modern scene environment preparation failed for '" +
                        preparation.Path + "': " + preparation.Error);
                }
                if (preparation.State != RuntimeEnvironmentState::Ready)
                    break;
                mStartupState = StartupState::StartRuntime;
            }
            [[fallthrough]];
            case StartupState::StartRuntime:
            {
                if (!mPendingScene)
                    throw std::logic_error("Game startup has no materialized scene candidate.");

                mPendingScene->OnViewportResize(
                    static_cast<std::uint32_t>(ConfigManager::mViewportSize.x),
                    static_cast<std::uint32_t>(ConfigManager::mViewportSize.y));
                {
                    HE_PROFILE_SECTION("RuntimeGameLayer::RuntimeStart");
                    mPendingScene->OnRuntimeStart();
                }
                mPendingRuntimeStarted = true;
                mStartupState = StartupState::PublishScene;
            }
            [[fallthrough]];
            case StartupState::PublishScene:
            {
                if (!mPendingScene || !mPendingRuntimeStarted)
                    throw std::logic_error("Game startup scene is not ready to publish.");

                application.RequestRuntimeStartupGameplayPresent();
#if defined(HE_DEV_AGENT_RUNTIME)
                const ApplicationRunOptions& runOptions = application.GetRunOptions();
                if (runOptions.HasAgentSession() && runOptions.bAgentStartPaused)
                    mPendingScene->SetRuntimePaused(true);
#endif
                mCurrentScene = std::move(mPendingScene);
                mPendingRuntimeStarted = false;
                BeginRenderResourceWarmup();
                mStartupState = StartupState::AwaitGameplayPresent;
                break;
            }
            case StartupState::AwaitGameplayPresent:
            {
                if (!application.HasRuntimeStartupGameplayPresentedFrame() || !PollRenderResourceWarmup())
                    break;
                mStartupState = StartupState::StartAgent;
            }
            [[fallthrough]];
            case StartupState::StartAgent:
            {
#if defined(HE_DEV_AGENT_RUNTIME)
                const ApplicationRunOptions& runOptions = application.GetRunOptions();
                if (runOptions.HasAgentSession())
                {
                    HE_PROFILE_SECTION("RuntimeGameLayer::AgentRuntimeStart");
                    DevAgentRuntimeConfig agentConfig;
                    agentConfig.SessionDirectory = runOptions.AgentSessionDirectory;
                    agentConfig.Token = runOptions.AgentToken;
                    agentConfig.SceneName = mDefaultScene;
                    agentConfig.bStartPaused = runOptions.bAgentStartPaused;
                    mAgentRuntime = CreateScope<DevAgentRuntime>();
                    if (!mAgentRuntime->Start(agentConfig, mCurrentScene))
                    {
                        throw std::runtime_error(
                            "Failed to start the requested Dev Agent Runtime session.");
                    }
                }
                if (!runOptions.InputReplayPath.empty())
                {
                    mInputReplay = CreateScope<InputReplayController>();
                    mInputReplay->Start(runOptions.InputReplayPath);
                }
#endif
                mStartupState = StartupState::Complete;
            }
            [[fallthrough]];
            case StartupState::Complete:
            {
                if (!mCurrentScene || mPendingRuntimeStarted)
                    throw std::logic_error("Game startup scene is not ready to complete.");

                HE_PROFILE_SECTION("RuntimeGameLayer::StartupReady");
                application.CompleteRuntimeStartup();
                (void)application.GetProjectResourceWarmup().ReleaseResidency(
                    RuntimeResourceWarmupResidency::Startup);
                SimulationClock::GetInstance().SetPaused(mCurrentScene->IsRuntimePaused());
                mStartupState = StartupState::Ready;
                HE_CORE_INFO(
                    "[Game] Ready scene={0} viewport={1}x{2}",
                    mDefaultScene,
                    static_cast<std::uint32_t>(ConfigManager::mViewportSize.x),
                    static_cast<std::uint32_t>(ConfigManager::mViewportSize.y));
                break;
            }
            case StartupState::Detached:
            case StartupState::Ready:
                break;
        }
    }

    void RuntimeGameLayer::BeginRenderResourceWarmup()
    {
        Application::GetInstance().SetRuntimeSceneLoading(true);
        const double now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        mRenderResourceWarmup.Begin(now, 0);
        mWarmupRenderedFrames = 0;
        bWarmupHasRenderWorld = false;
        mWarmupPeakTextures = 0;
        mWarmupLastMeshUploads = Application::GetInstance().GetMainSceneStats().UploadedMeshCount;
        mSceneMeshWarmups.clear();
        if (mCurrentScene)
            if (auto* renderer = mCurrentScene->TryGetSystem<RenderSystem3D>())
                mSceneMeshWarmups = renderer->PrepareLevelMeshResources();
        HE_CORE_INFO("[Game] Render resource warmup begin scene={} simulationFrame={} residentMeshes={}",
            mDefaultScene, SimulationClock::GetInstance().GetFrameNumber(), mSceneMeshWarmups.size());
    }

    bool RuntimeGameLayer::PollRenderResourceWarmup()
    {
        const auto& stats = Application::GetInstance().GetMainSceneStats();
        mWarmupPeakTextures = std::max(mWarmupPeakTextures, stats.PendingTextureUploads);
        const double now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const bool bMeshesStable = stats.UploadedMeshCount == mWarmupLastMeshUploads;
        mWarmupLastMeshUploads = stats.UploadedMeshCount;
        // Empty/2D-only levels can have no submitted RenderWorld at all. Such
        // levels have no scene GPU work to await; do not depend on stale 3D stats.
        const bool bReady = !bWarmupHasRenderWorld || (stats.bCpuCounterFrameValid &&
            stats.PendingTextureUploads == 0 && bMeshesStable && stats.GraphBuildReasonMask == 0);
        const auto state = mRenderResourceWarmup.Observe(bReady, mWarmupRenderedFrames, now);
        if (state == RenderResourceWarmupState::TimedOut)
            throw std::runtime_error("Scene render resource warmup timed out after 90 seconds: " + mDefaultScene);
        if (state != RenderResourceWarmupState::Ready) return false;
        mSceneMeshWarmups.clear();
        Application::GetInstance().SetRuntimeSceneLoading(false);
        HE_CORE_INFO("[Game] Render resource warmup ready scene={} pendingTextures={} worldFrame={} simulationFrame={}",
            mDefaultScene, stats.PendingTextureUploads, stats.CpuCounterWorldFrame,
            SimulationClock::GetInstance().GetFrameNumber());
        return true;
    }

    void RuntimeGameLayer::OnDetach()
    {
        if (mCurrentScene)
            if (auto* scripts = mCurrentScene->TryGetSystem<ScriptSystem>()) scripts->NotifyApplicationQuit();
        ResetStartupAndRuntime();
        if (!mLoadingScreenDocument.empty())
            RmlUiManager::GetInstance().UnloadDocument(mLoadingScreenDocument);
        mLoadingScreenDocument.clear();
        mLoadingScreenVisible = false;

        mRuntimeAltDown = false;
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        SetRuntimeCursorCaptured(false);

        if (auto* imguiLayer = Application::GetInstance().GetImGuiLayer())
            imguiLayer->BlockEvents(true);
    }

	void RuntimeGameLayer::BeginRuntimeSceneTransition(std::string scenePath, bool bForceReload)
	{
		if (mRuntimeSceneTransitionState != RuntimeSceneTransitionState::Idle)
			throw std::logic_error("A runtime scene transition is already active.");
		if (scenePath == mDefaultScene && !bForceReload)
		{
			HE_CORE_INFO("[Game] Ignoring runtime scene request for active scene={0}", scenePath);
			return;
		}

		RuntimeResourceRegistry& registry = RuntimeResourceRegistry::GetInstance();
		if (!registry.IsInitialized())
			throw std::logic_error("Runtime scene transition requires an initialized resource registry.");

		SceneDocumentPath documentPath = ResolveSceneDocumentPath(
			ConfigManager::GetInstance().GetRootFolder(),
			scenePath);
		const std::shared_ptr<const RuntimeResourceCatalogSnapshot> catalog =
			RuntimeResourceCatalog::GetInstance().GetSnapshot();
		if (catalog && !catalog->IsEmpty())
		{
			const RuntimeResourceCatalogEntry* entry = catalog->Find(documentPath.Key.Path);
			if (!entry || entry->Type != RuntimeCatalogResourceType::Scene)
			{
				throw std::invalid_argument(
					"Runtime scene is absent from the resource catalog: " + scenePath);
			}
		}

		if (bForceReload)
			(void)registry.Invalidate(documentPath.Key);
		RuntimeResourceGroup documentGroup = registry.CreateGroup();
		(void)QueueSceneDocument(registry, documentPath, documentGroup);

		mRuntimeSceneTransitionPath = std::move(scenePath);
		mRuntimeSceneDocumentPath = std::move(documentPath);
		mRuntimeSceneDocumentGroup = std::move(documentGroup);
		mRuntimeSceneTransitionState = RuntimeSceneTransitionState::WaitSceneDocument;
        RefreshLoadingScreenSettings();
        Application::GetInstance().SetRuntimeSceneLoading(true);
		HE_CORE_INFO(
			"[Game] Runtime scene transition queued from={0} to={1}",
			mDefaultScene,
			mRuntimeSceneTransitionPath);
	}

	void RuntimeGameLayer::AdvanceRuntimeSceneTransition()
	{
		Application& application = Application::GetInstance();
		RuntimeResourceRegistry& registry = RuntimeResourceRegistry::GetInstance();

		switch (mRuntimeSceneTransitionState)
		{
			case RuntimeSceneTransitionState::WaitSceneDocument:
			{
				const RuntimeResourceState state = registry.GetState(mRuntimeSceneDocumentPath.Key);
				if (state == RuntimeResourceState::Failed)
				{
					(void)AcquireSceneDocument(registry, mRuntimeSceneDocumentPath);
					throw std::runtime_error(
						"Runtime scene document load failed without reporting an exception: " +
						mRuntimeSceneTransitionPath);
				}
				if (state == RuntimeResourceState::Unloaded ||
					state == RuntimeResourceState::EvictPending)
				{
					throw std::runtime_error(
						"Runtime scene document became unavailable: " +
						mRuntimeSceneTransitionPath);
				}
				if (state != RuntimeResourceState::Ready)
					break;

				RuntimeResourceHandle<SceneDocument> handle =
					registry.TryAcquireReady<SceneDocument>(mRuntimeSceneDocumentPath.Key);
				if (!handle)
					throw std::runtime_error("Ready runtime scene document could not be acquired.");
				std::shared_ptr<const SceneDocument> document = handle.GetShared();
				if (!document)
					throw std::runtime_error("Ready runtime scene document lease could not be created.");

				mRuntimeSceneDocumentGroup.Release();
				mRuntimeSceneMaterializer.Start(
					registry,
					std::move(document),
					mRuntimeSceneDocumentPath.ProjectRoot,
					RuntimeResourceCatalog::GetInstance().GetSnapshot());
				const SceneMaterializationProgress progress =
					mRuntimeSceneMaterializer.GetProgress();
				if (progress.State == SceneMaterializationState::Failed ||
					progress.State == SceneMaterializationState::Cancelled)
				{
					mRuntimeSceneMaterializer.RethrowFailure();
					throw std::runtime_error("Runtime scene materializer failed to start.");
				}
				mRuntimeSceneTransitionState = RuntimeSceneTransitionState::MaterializeScene;
			}
			[[fallthrough]];
			case RuntimeSceneTransitionState::MaterializeScene:
			{
				const SceneMaterializationProgress progress = mRuntimeSceneMaterializer.Pump({
					std::chrono::milliseconds(2),
					16
				});
				if (progress.State == SceneMaterializationState::Failed ||
					progress.State == SceneMaterializationState::Cancelled)
				{
					mRuntimeSceneMaterializer.RethrowFailure();
					throw std::runtime_error("Runtime scene materialization failed.");
				}
				if (progress.State == SceneMaterializationState::Ready)
				{
					mRuntimeSceneCandidate = mRuntimeSceneMaterializer.TakeCompletedLevel();
					if (!mRuntimeSceneCandidate)
						throw std::runtime_error("Runtime scene materialization produced no Level.");
					mRuntimeSceneTransitionState =
						RuntimeSceneTransitionState::PrepareRuntimeResources;
				}
				else if (progress.State == SceneMaterializationState::Empty)
				{
					throw std::logic_error("Runtime scene materializer lost its active state.");
				}
				else
				{
					break;
				}
			}
			[[fallthrough]];
			case RuntimeSceneTransitionState::PrepareRuntimeResources:
			{
				if (!mRuntimeSceneCandidate)
					throw std::logic_error("Runtime scene transition has no materialized candidate.");
				const std::string environmentPath = FindEnvironmentPath(*mRuntimeSceneCandidate);
				const RuntimeEnvironmentPreparation preparation =
					application.PrepareRenderEnvironment(environmentPath);
				if (preparation.State == RuntimeEnvironmentState::Failed)
				{
					throw std::runtime_error(
						"Runtime scene environment preparation failed for '" +
						preparation.Path + "': " + preparation.Error);
				}
				if (preparation.State != RuntimeEnvironmentState::Ready)
					break;
				mRuntimeSceneTransitionState = RuntimeSceneTransitionState::Commit;
			}
			[[fallthrough]];
			case RuntimeSceneTransitionState::Commit:
			{
				if (!mCurrentScene || !mRuntimeSceneCandidate)
					throw std::logic_error("Runtime scene transition cannot commit without both Levels.");

				mRuntimeSceneCandidate->OnViewportResize(
					static_cast<std::uint32_t>(ConfigManager::mViewportSize.x),
					static_cast<std::uint32_t>(ConfigManager::mViewportSize.y));
				Ref<Level> previousScene = mCurrentScene;
				try
				{
					previousScene->OnRuntimeStop();
				}
				catch (...)
				{
					const std::exception_ptr stopFailure = std::current_exception();
					try
					{
						previousScene->OnRuntimeStart();
					}
					catch (...)
					{
						ReportCleanupFailure(
							"Failed to restart the previous scene after its stop failed.",
							std::current_exception());
					}
					std::rethrow_exception(stopFailure);
				}
				try
				{
					mRuntimeSceneCandidate->OnRuntimeStart();
				}
				catch (...)
				{
					const std::exception_ptr startFailure = std::current_exception();
					try
					{
						previousScene->OnRuntimeStart();
					}
					catch (...)
					{
						ReportCleanupFailure(
							"Failed to restart the previous scene after a transition failure.",
							std::current_exception());
					}
					std::rethrow_exception(startFailure);
				}

				mCurrentScene = std::move(mRuntimeSceneCandidate);
				mDefaultScene = std::move(mRuntimeSceneTransitionPath);
				mRuntimeSceneDocumentPath = {};
                BeginRenderResourceWarmup();
                mRuntimeSceneTransitionState = RuntimeSceneTransitionState::WarmRenderResources;
                break;
            }
            case RuntimeSceneTransitionState::WarmRenderResources:
            {
                if (!PollRenderResourceWarmup()) break;
                mRuntimeSceneTransitionState = RuntimeSceneTransitionState::Idle;
#if defined(HE_DEV_AGENT_RUNTIME)
				if (mAgentRuntime)
					mAgentRuntime->SetLevel(mCurrentScene, mDefaultScene);
#endif
				SimulationClock::GetInstance().SetPaused(mCurrentScene->IsRuntimePaused());
				HE_CORE_INFO("[Game] Runtime scene transition complete scene={0}", mDefaultScene);
				break;
			}
			case RuntimeSceneTransitionState::Idle:
				break;
		}
	}

	void RuntimeGameLayer::AbortRuntimeSceneTransition(const std::exception_ptr& failure) noexcept
	{
        mSceneMeshWarmups.clear();
        Application::GetInstance().SetRuntimeSceneLoading(false);
		ReportCleanupFailure("Runtime scene transition failed.", failure);
		try
		{
			mRuntimeSceneMaterializer.RequestCancel();
		}
		catch (...)
		{
			ReportCleanupFailure(
				"Failed to cancel runtime scene materialization.",
				std::current_exception());
		}
		mRuntimeSceneDocumentGroup.Release();
		mRuntimeSceneCandidate.reset();
		mRuntimeSceneDocumentPath = {};
		mRuntimeSceneTransitionPath.clear();
		mRuntimeSceneTransitionState = RuntimeSceneTransitionState::Idle;
	}

    void RuntimeGameLayer::ResetStartupAndRuntime() noexcept
    {
        mSceneMeshWarmups.clear();
        Application::GetInstance().SetRuntimeSceneLoading(false);
#if defined(HE_DEV_AGENT_RUNTIME)
        if (mInputReplay)
        {
            try
            {
                mInputReplay->Stop();
            }
            catch (...)
            {
                ReportCleanupFailure(
                    "Failed to stop input replay during game cleanup.",
                    std::current_exception());
            }
            mInputReplay.reset();
        }
        if (mAgentRuntime)
        {
            try
            {
                mAgentRuntime->Stop();
            }
            catch (...)
            {
                ReportCleanupFailure(
                    "Failed to stop Dev Agent Runtime during game cleanup.",
                    std::current_exception());
            }
            mAgentRuntime.reset();
        }
#endif

        if (mPendingRuntimeStarted && mPendingScene)
        {
            try
            {
                mPendingScene->OnRuntimeStop();
            }
            catch (...)
            {
                ReportCleanupFailure(
                    "Failed to stop the pending game scene during cleanup.",
                    std::current_exception());
            }
        }
        mPendingRuntimeStarted = false;
		try
		{
			mRuntimeSceneMaterializer.RequestCancel();
		}
		catch (...)
		{
			ReportCleanupFailure(
				"Failed to cancel runtime scene materialization during cleanup.",
				std::current_exception());
		}
		mRuntimeSceneDocumentGroup.Release();
		mRuntimeSceneCandidate.reset();
		mRuntimeSceneDocumentPath = {};
		mRuntimeSceneTransitionPath.clear();
		mRuntimeSceneTransitionState = RuntimeSceneTransitionState::Idle;

        if (mCurrentScene)
        {
            try
            {
                mCurrentScene->OnRuntimeStop();
            }
            catch (...)
            {
                ReportCleanupFailure(
                    "Failed to stop the active game scene during cleanup.",
                    std::current_exception());
            }
        }

        try
        {
            mSceneMaterializer.RequestCancel();
        }
        catch (...)
        {
            ReportCleanupFailure(
                "Failed to cancel scene materialization during cleanup.",
                std::current_exception());
        }

        mPendingScene.reset();
        mCurrentScene.reset();
        Application::GetInstance().SubmitRenderWorld({});
        mSceneDocumentGroup.Release();
        mSceneDocumentPath = {};
        mDefaultScene.clear();
        mStartupState = StartupState::Detached;
		Application::GetInstance().ClearRuntimeSceneRequest();
        Application::GetInstance().CancelRuntimeStartup();
        SimulationClock::GetInstance().SetPaused(false);
    }

    void RuntimeGameLayer::OnPreUpdate()
    {
        if (mStartupState == StartupState::Detached)
            return;
        try
        {
            if (mStartupState != StartupState::Ready)
                AdvanceStartup();
        }
        catch (...)
        {
            const std::exception_ptr startupFailure = std::current_exception();
            ResetStartupAndRuntime();
            std::rethrow_exception(startupFailure);
        }
        if (mStartupState != StartupState::Ready)
            return;

		Application& application = Application::GetInstance();
		if (mRuntimeSceneTransitionState == RuntimeSceneTransitionState::Idle)
		{
			bool bForceReload = false;
			if (std::optional<std::string> request =
				application.ConsumeRuntimeSceneRequest(&bForceReload))
			{
				try
				{
					BeginRuntimeSceneTransition(std::move(*request), bForceReload);
				}
				catch (...)
				{
					AbortRuntimeSceneTransition(std::current_exception());
				}
			}
		}
		if (mRuntimeSceneTransitionState != RuntimeSceneTransitionState::Idle)
		{
			try
			{
				AdvanceRuntimeSceneTransition();
			}
			catch (...)
			{
                const bool bCommittedWarmup = mRuntimeSceneTransitionState ==
                    RuntimeSceneTransitionState::WarmRenderResources;
				AbortRuntimeSceneTransition(std::current_exception());
                // A committed level cannot roll back after its old runtime was
                // stopped. Propagate a failed warmup instead of playing a broken level.
                if (bCommittedWarmup) throw;
			}
		}

        if (mRuntimeSceneTransitionState != RuntimeSceneTransitionState::Idle)
        {
            SimulationClock::GetInstance().SetPaused(true);
            return;
        }

#if defined(HE_DEV_AGENT_RUNTIME)
        if (mInputReplay)
            mInputReplay->Advance();
        if (mAgentRuntime)
        {
            mAgentRuntime->Tick();
            if (mAgentRuntime->ConsumeStopRequested())
            {
                SimulationClock::GetInstance().SetPaused(false);
                Application::GetInstance().Close();
                return;
            }

            mAgentRuntime->PrepareSimulationFrame(!Application::GetInstance().IsMinimized());
        }
#endif
        SimulationClock::GetInstance().SetPaused(
            mCurrentScene && mCurrentScene->IsRuntimePaused());
    }

    void RuntimeGameLayer::OnUpdate(Timestep ts)
    {
        HE_PROFILE_SECTION("RuntimeGameLayer::OnUpdate");

        UpdateRuntimeCursorPolicy();

        Application& application = Application::GetInstance();
        // GLFW pointers and ImGui use client coordinates, which differ from
        // framebuffer pixels on Retina displays. Keep layout and hit testing in
        // that same logical space; only the Rml renderer needs the pixel extent.
        const float viewportWidth = static_cast<float>(Application::GetWindow().GetWidth());
        const float viewportHeight = static_cast<float>(Application::GetWindow().GetHeight());
        const float renderWidth = static_cast<float>(application.GetRenderWidth());
        const float renderHeight = static_cast<float>(application.GetRenderHeight());
        const float uiScale = application.GetImGuiLayer() ? application.GetImGuiLayer()->GetUIScale() : 1.0f;
        ScriptInputContext::Region = {true, true, true, 0.0f, 0.0f, viewportWidth, viewportHeight};
        UIViewportContext::Set(0.0f, 0.0f, viewportWidth, viewportHeight, uiScale);
        auto& rmlUi = RmlUiManager::GetInstance();
        rmlUi.SetViewportRect(0.0f, 0.0f, viewportWidth, viewportHeight, uiScale);
        {
            HE_PROFILE_SECTION("RmlUi::BeginFrame");
            rmlUi.BeginFrame(viewportWidth, viewportHeight, uiScale, renderWidth, renderHeight);
        }

        UpdateLoadingScreen();
        if (!mCurrentScene)
        {
            rmlUi.Update();
            return;
        }

        // Initial and replacement levels must render their material/mesh uploads
        // behind the loading overlay without advancing scripts, physics or timers.
        if (mStartupState != StartupState::Ready ||
            mRuntimeSceneTransitionState != RuntimeSceneTransitionState::Idle)
        {
            RenderPrimitiveCollector collector;
            collector.BeginFrame();
            Renderer2D::CollectorScope collectorScope(collector);
            try
            {
                if (auto* renderer = mCurrentScene->TryGetSystem<RenderSystem3D>()) renderer->OnUpdateRuntime(Timestep(0.0f));
                if (auto* renderer = mCurrentScene->TryGetSystem<RenderSystem2D>()) renderer->OnUpdateRuntime(Timestep(0.0f));
                collector.EndFrame();
            }
            catch (...) { collector.CancelFrame(); throw; }
            auto warmupWorld = AcquireMergedRenderWorld(*mCurrentScene);
            bWarmupHasRenderWorld = static_cast<bool>(warmupWorld);
            application.SubmitRenderWorld(std::move(warmupWorld));
            ++mWarmupRenderedFrames;
            rmlUi.Update();
            return;
        }

        {
            HE_PROFILE_SECTION("RuntimeGameLayer::SceneUpdateRuntime");
#if defined(HE_DEV_AGENT_RUNTIME)
            if (mAgentRuntime && mAgentRuntime->GetAuthorPreview())
            {
                rmlUi.SuppressRenderingForCurrentFrame();
                const auto preview = mAgentRuntime->GetAuthorPreview();
                RenderPrimitiveCollector collector;
                collector.BeginFrame();
                Renderer2D::CollectorScope collectorScope(collector);
                try
                {
                    if (auto* renderer = preview->TryGetSystem<RenderSystem3D>()) renderer->OnUpdateRuntime(ts);
                    if (auto* renderer = preview->TryGetSystem<RenderSystem2D>()) renderer->OnUpdateRuntime(ts);
                    collector.EndFrame();
                }
                catch (...) { collector.CancelFrame(); throw; }
                application.SubmitRenderWorld(AcquireMergedRenderWorld(*preview));
                mAgentRuntime->ProcessPendingCapture();
                mAgentRuntime->CompleteSimulationFrame();
                return;
            }
#endif
            mCurrentScene->OnUpdateRuntime(ts);
        }
		application.SubmitRenderWorld(AcquireMergedRenderWorld(*mCurrentScene));
        UpdateRuntimeCursorPolicy();
        {
            HE_PROFILE_SECTION("RmlUi::Update");
            rmlUi.Update();
        }
#if defined(HE_DEV_AGENT_RUNTIME)
        if (mAgentRuntime)
        {
			mAgentRuntime->ProcessPendingCapture();
            mAgentRuntime->CompleteSimulationFrame();
            SimulationClock::GetInstance().SetPaused(mCurrentScene->IsRuntimePaused());
        }
#endif
    }

    void RuntimeGameLayer::GetLoadingScreenProgress(const char*& stage, float& progress) const
    {
        const auto localized = [this](const char* chinese, const char* english) {
            return mLoadingScreenChinese ? chinese : english;
        };
        stage = localized("准备启程", "Getting ready");
        progress = 0.0f;
        switch (mStartupState)
        {
            case StartupState::AwaitFirstPresent:
            case StartupState::LoadGameConfig:
            case StartupState::QueueSceneDocument:
                stage = localized("开启旅程", "Opening your journey");
                break;
            case StartupState::WaitBootResources:
            {
                stage = localized("唤醒世界", "Awakening the world");
                const auto warmupProgress = Application::GetInstance().GetProjectResourceWarmup()
                    .GetGroupProgress("Boot");
                if (warmupProgress)
                    progress = warmupProgress->Ratio * 0.15f;
                break;
            }
            case StartupState::WaitSceneDocument:
                stage = localized("读取目的地", "Reading the destination");
                if (mSceneDocumentGroup)
                    progress = mSceneDocumentGroup.Progress().Ratio * 0.15f;
                break;
            case StartupState::MaterializeScene:
            {
                stage = localized("构筑场景", "Building the scene");
                const SceneMaterializationProgress sceneProgress =
                    mSceneMaterializer.GetProgress();
                progress = 0.15f + sceneProgress.Ratio * 0.55f;
                break;
            }
            case StartupState::PrepareRuntimeResources:
            {
                stage = localized("点亮光影", "Bringing light to life");
                progress = 0.85f;
                break;
            }
            case StartupState::StartRuntime:
            case StartupState::PublishScene:
            case StartupState::AwaitGameplayPresent:
            case StartupState::StartAgent:
            case StartupState::Complete:
                stage = localized("即将就绪", "Almost ready");
                progress = 0.98f;
                break;
            case StartupState::Detached:
            case StartupState::Ready:
                break;
        }

        if (mRuntimeSceneTransitionState != RuntimeSceneTransitionState::Idle)
        {
            stage = localized("读取目的地", "Reading the destination");
            progress = mRuntimeSceneDocumentGroup ? mRuntimeSceneDocumentGroup.Progress().Ratio * 0.15f : 0.0f;
            if (mRuntimeSceneTransitionState == RuntimeSceneTransitionState::MaterializeScene)
            {
                stage = localized("构筑场景", "Building the scene");
                progress = 0.15f + mRuntimeSceneMaterializer.GetProgress().Ratio * 0.55f;
            }
            else if (mRuntimeSceneTransitionState == RuntimeSceneTransitionState::PrepareRuntimeResources ||
                mRuntimeSceneTransitionState == RuntimeSceneTransitionState::Commit)
            {
                stage = localized("点亮光影", "Bringing light to life");
                progress = 0.85f;
            }
        }
        if (mStartupState == StartupState::AwaitGameplayPresent ||
            mRuntimeSceneTransitionState == RuntimeSceneTransitionState::WarmRenderResources)
        {
            stage = localized("呈现最后的细节", "Preparing the final details");
            const auto pending = Application::GetInstance().GetMainSceneStats().PendingTextureUploads;
            progress = 0.85f + 0.14f * (1.0f - static_cast<float>(pending) /
                static_cast<float>(std::max(mWarmupPeakTextures, 1u)));
        }
        progress = std::clamp(progress, 0.0f, 1.0f);
    }

    void RuntimeGameLayer::UpdateLoadingScreen()
    {
        auto& ui = RmlUiManager::GetInstance();
        if (!ui.IsInitialized())
            return;
        const bool loading = (mStartupState != StartupState::Ready && mStartupState != StartupState::Detached) ||
            mRuntimeSceneTransitionState != RuntimeSceneTransitionState::Idle;
        if (!loading || !mCopperLoadingScreen)
        {
            if (mLoadingScreenVisible)
                ui.HideDocument(mLoadingScreenDocument);
            mLoadingScreenVisible = false;
            return;
        }
        if (mLoadingScreenDocument.empty() &&
            !ui.LoadDocument(mLoadingScreenDocumentPath, mLoadingScreenOwner, &mLoadingScreenDocument))
            throw std::runtime_error("Could not load the configured runtime loading screen: " + mLoadingScreenDocumentPath);
        const auto localized = [this](const char* chinese, const char* english) {
            return mLoadingScreenChinese ? chinese : english;
        };
        ui.ShowOverlayDocument(mLoadingScreenDocument);
        mLoadingScreenVisible = true;
        const char* stage;
        float progress;
        GetLoadingScreenProgress(stage, progress);
        ui.SetElementText(mLoadingScreenDocument, "loading-brand", mLoadingScreenTitle);
        ui.SetElementText(mLoadingScreenDocument, "loading-kicker", localized("旅程 / 正在准备", "YOUR JOURNEY / IN PREPARATION"));
        ui.SetElementText(mLoadingScreenDocument, "loading-title", localized("即将启程", "ON THE WAY"));
        ui.SetElementText(mLoadingScreenDocument, "loading-caption", localized("片刻之后，步入光影之间。", "A moment away from a world of light and shadow."));
        ui.SetElementText(mLoadingScreenDocument, "loading-stage", stage);
        ui.SetElementText(mLoadingScreenDocument, "loading-footer", localized("场景 · 人物 · 光影", "SCENE / CHARACTER / LIGHT"));
        ui.SetElementText(mLoadingScreenDocument, "loading-hint", localized("正在准备，请稍候", "PREPARING YOUR EXPERIENCE"));
        const std::string percentage = std::to_string(static_cast<int>(progress * 100.0f)) + "%";
        ui.SetElementText(mLoadingScreenDocument, "loading-percent", percentage);
        ui.SetElementStyle(mLoadingScreenDocument, "loading-progress", "width", percentage);
    }

    void RuntimeGameLayer::OnImGuiRender()
    {
        const bool bLoading = (mStartupState != StartupState::Ready &&
            mStartupState != StartupState::Detached) ||
            mRuntimeSceneTransitionState != RuntimeSceneTransitionState::Idle;
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration         | ImGuiWindowFlags_NoDocking           |
            ImGuiWindowFlags_NoTitleBar           | ImGuiWindowFlags_NoCollapse          |
            ImGuiWindowFlags_NoResize             | ImGuiWindowFlags_NoMove              |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus         |
            ImGuiWindowFlags_NoBackground;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,  0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,   ImVec2(0.0f, 0.0f));
        ImGui::Begin("##GameViewport", nullptr, flags);

        ImVec2 size = ImGui::GetContentRegionAvail();
        if (size.x > 0.0f && size.y > 0.0f)
        {
            const ImVec2 imageOrigin = ImGui::GetCursorScreenPos();
            const std::uint32_t width = static_cast<std::uint32_t>(size.x);
            const std::uint32_t height = static_cast<std::uint32_t>(size.y);
            if (static_cast<std::uint32_t>(ConfigManager::mViewportSize.x) != width ||
                static_cast<std::uint32_t>(ConfigManager::mViewportSize.y) != height)
            {
                ConfigManager::mViewportSize = { size.x, size.y };
                if (mCurrentScene)
                    mCurrentScene->OnViewportResize(width, height);
            }

            if (bLoading && !mCopperLoadingScreen &&
                size.x >= 220.0f && size.y >= 80.0f)
            {
                const char* stage;
                float progress;
                GetLoadingScreenProgress(stage, progress);
                const float panelWidth = std::min(size.x - 40.0f, 420.0f);
                constexpr float panelHeight = 64.0f;
                const ImVec2 panelMin{
                    imageOrigin.x + (size.x - panelWidth) * 0.5f,
                    imageOrigin.y + (size.y - panelHeight) * 0.5f
                };
                const ImVec2 panelMax{ panelMin.x + panelWidth, panelMin.y + panelHeight };
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                drawList->AddRectFilled(
                    panelMin,
                    panelMax,
                    IM_COL32(12, 16, 24, 220),
                    6.0f);
                drawList->AddText(
                    { panelMin.x + 14.0f, panelMin.y + 12.0f },
                    IM_COL32(235, 240, 248, 255),
                    stage);
                const ImVec2 barMin{ panelMin.x + 14.0f, panelMax.y - 18.0f };
                const ImVec2 barMax{ panelMax.x - 14.0f, panelMax.y - 10.0f };
                drawList->AddRectFilled(barMin, barMax, IM_COL32(45, 52, 66, 255), 4.0f);
                drawList->AddRectFilled(
                    barMin,
                    { barMin.x + (barMax.x - barMin.x) * progress, barMax.y },
                    IM_COL32(87, 166, 255, 255),
                    4.0f);
            }
        }

        ImGui::End();
        ImGui::PopStyleVar(3);

        if (mCurrentScene && !bLoading)
        {
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const float uiScale = Application::GetInstance().GetImGuiLayer() ? Application::GetInstance().GetImGuiLayer()->GetUIScale() : 1.0f;
            UIViewportContext::Set(vp->WorkPos.x, vp->WorkPos.y, vp->WorkSize.x, vp->WorkSize.y, uiScale);
            mCurrentScene->OnImGuiRender();
        }
    }

    void RuntimeGameLayer::OnEvent(Event& e)
    {
        static const bool traceInput = [] { const char* value = std::getenv("HE_TRACE_UI_INPUT"); return value && std::string_view(value) == "1"; }();
        if (traceInput && (e.GetEventType() == EventType::MouseButtonPressed || e.GetEventType() == EventType::MouseButtonReleased))
        {
            const auto raw = Input::GetMousePosition();
            const auto& input = ScriptInputContext::Region;
            HE_CORE_INFO("[UIInput] game event={} handled={} startup={} transition={} raw={},{} logical={}x{} physical={}x{} ui={},{},{},{} input={},{},{},{} captured={}",
                e.GetName(), e.Handled, static_cast<int>(mStartupState), static_cast<int>(mRuntimeSceneTransitionState),
                raw.x, raw.y, Application::GetWindow().GetWidth(), Application::GetWindow().GetHeight(),
                Application::GetInstance().GetRenderWidth(), Application::GetInstance().GetRenderHeight(),
                UIViewportContext::X, UIViewportContext::Y, UIViewportContext::W, UIViewportContext::H,
                input.X, input.Y, input.Width, input.Height, Application::GetWindow().IsMouseCursorCaptured());
        }
        if (mStartupState == StartupState::Ready && mCurrentScene &&
            mRuntimeSceneTransitionState == RuntimeSceneTransitionState::Idle)
            RmlUiManager::GetInstance().OnEvent(e);
    }

    void RuntimeGameLayer::UpdateRuntimeCursorPolicy()
    {
        const bool bAltDown = Input::IsKeyPressed(Key::LeftAlt) || Input::IsKeyPressed(Key::RightAlt);
        const bool bUiCursorVisible = RmlUiManager::GetInstance().IsMouseCursorVisibleRequested();
        if (bUiCursorVisible)
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        else
            ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        mRuntimeAltDown = bAltDown;
        const bool bCaptureAllowed = !Application::GetInstance().GetRunOptions().bNoCursorCapture;
        SetRuntimeCursorCaptured(bCaptureAllowed && !bAltDown && !bUiCursorVisible);
    }

    void RuntimeGameLayer::SetRuntimeCursorCaptured(bool bCaptured)
    {
        if (mRuntimeCursorCaptured == bCaptured)
            return;

        if (bCaptured)
            CenterCursorToWindow();

        Application::GetWindow().SetMouseCursorCaptured(bCaptured);

        mRuntimeCursorCaptured = bCaptured;
    }

    void RuntimeGameLayer::CenterCursorToWindow()
    {
        Application::GetWindow().CenterMouseCursor();
    }
}
