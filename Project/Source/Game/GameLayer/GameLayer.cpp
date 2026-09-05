#include "GameLayer.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/Profile/Profile.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/RuntimeProjectResourceWarmup.h"
#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceCatalog.h"
#include "Runtime/EcsFramework/Serializer/SceneSerializer.h"
#include "Runtime/EcsFramework/Component/Light/EnvironmentLightComponent.h"
#include "Runtime/EcsFramework/System/Render/RenderSystem2D.h"
#include "Runtime/EcsFramework/System/Render/RenderSystem3D.h"
#include "Runtime/Graphics/Renderer/RuntimeRenderer.h"
#include "Runtime/EcsFramework/System/UI/UIViewportContext.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Core/Input/Input.h"
#include "Runtime/Core/Input/KeyCodes.h"
#include "Runtime/Core/SimulationClock.h"
#include "Runtime/UI/Rml/RmlUiManager.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <chrono>
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

    GameLayer::GameLayer()
    {
        HE_CORE_INFO("Game Begin!");
    }

    void GameLayer::OnAttach()
    {
        HE_PROFILE_SECTION("GameLayer::OnAttach");

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

    void GameLayer::AdvanceStartup()
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
                HE_PROFILE_SECTION("GameLayer::LoadGameConfig");
                RuntimeProjectResourceWarmup& warmup = application.GetProjectResourceWarmup();
                mDefaultScene = warmup.GetDefaultScene();
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
                    HE_PROFILE_SECTION("GameLayer::RuntimeStart");
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
                mStartupState = StartupState::AwaitGameplayPresent;
                break;
            }
            case StartupState::AwaitGameplayPresent:
            {
                if (!application.HasRuntimeStartupGameplayPresentedFrame())
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
                    HE_PROFILE_SECTION("GameLayer::AgentRuntimeStart");
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
#endif
                mStartupState = StartupState::Complete;
            }
            [[fallthrough]];
            case StartupState::Complete:
            {
                if (!mCurrentScene || mPendingRuntimeStarted)
                    throw std::logic_error("Game startup scene is not ready to complete.");

                HE_PROFILE_SECTION("GameLayer::StartupReady");
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

    void GameLayer::OnDetach()
    {
        ResetStartupAndRuntime();

        mRuntimeAltDown = false;
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        SetRuntimeCursorCaptured(false);

        if (auto* imguiLayer = Application::GetInstance().GetImGuiLayer())
            imguiLayer->BlockEvents(true);
    }

	void GameLayer::BeginRuntimeSceneTransition(std::string scenePath)
	{
		if (mRuntimeSceneTransitionState != RuntimeSceneTransitionState::Idle)
			throw std::logic_error("A runtime scene transition is already active.");
		if (scenePath == mDefaultScene)
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

		RuntimeResourceGroup documentGroup = registry.CreateGroup();
		(void)QueueSceneDocument(registry, documentPath, documentGroup);

		mRuntimeSceneTransitionPath = std::move(scenePath);
		mRuntimeSceneDocumentPath = std::move(documentPath);
		mRuntimeSceneDocumentGroup = std::move(documentGroup);
		mRuntimeSceneTransitionState = RuntimeSceneTransitionState::WaitSceneDocument;
		HE_CORE_INFO(
			"[Game] Runtime scene transition queued from={0} to={1}",
			mDefaultScene,
			mRuntimeSceneTransitionPath);
	}

	void GameLayer::AdvanceRuntimeSceneTransition()
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

	void GameLayer::AbortRuntimeSceneTransition(const std::exception_ptr& failure) noexcept
	{
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

    void GameLayer::ResetStartupAndRuntime() noexcept
    {
#if defined(HE_DEV_AGENT_RUNTIME)
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

    void GameLayer::OnPreUpdate()
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
			if (std::optional<std::string> request =
				application.ConsumeRuntimeSceneRequest())
			{
				try
				{
					BeginRuntimeSceneTransition(std::move(*request));
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
				AbortRuntimeSceneTransition(std::current_exception());
			}
		}

#if defined(HE_DEV_AGENT_RUNTIME)
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

    void GameLayer::OnUpdate(Timestep ts)
    {
        HE_PROFILE_SECTION("GameLayer::OnUpdate");

        if (!mCurrentScene)
            return;

        UpdateRuntimeCursorPolicy();

        Application& application = Application::GetInstance();
        const float viewportWidth = static_cast<float>(application.GetRenderWidth());
        const float viewportHeight = static_cast<float>(application.GetRenderHeight());
        const float uiScale = Application::GetInstance().GetImGuiLayer() ? Application::GetInstance().GetImGuiLayer()->GetUIScale() : 1.0f;
        UIViewportContext::Set(0.0f, 0.0f, viewportWidth, viewportHeight, uiScale);
        auto& rmlUi = RmlUiManager::GetInstance();
        rmlUi.SetViewportRect(0.0f, 0.0f, viewportWidth, viewportHeight, uiScale);
        {
            HE_PROFILE_SECTION("RmlUi::BeginFrame");
            rmlUi.BeginFrame(viewportWidth, viewportHeight, uiScale);
        }

        {
            HE_PROFILE_SECTION("GameLayer::SceneUpdateRuntime");
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

    void GameLayer::OnImGuiRender()
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
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

#if defined(WITH_EDITOR)
            if (mStartupState != StartupState::Ready &&
                mStartupState != StartupState::Detached &&
                size.x >= 220.0f && size.y >= 80.0f)
            {
                const char* stage = "Starting engine...";
                float progress = 0.0f;
                switch (mStartupState)
                {
                    case StartupState::AwaitFirstPresent:
                    case StartupState::LoadGameConfig:
                    case StartupState::QueueSceneDocument:
                        stage = "Loading project...";
                        break;
                    case StartupState::WaitBootResources:
                    {
                        stage = "Warming up content...";
                        const auto warmupProgress = Application::GetInstance().GetProjectResourceWarmup()
                            .GetGroupProgress("Boot");
                        if (warmupProgress)
                            progress = warmupProgress->Ratio * 0.15f;
                        break;
                    }
                    case StartupState::WaitSceneDocument:
                        stage = "Reading scene...";
                        if (mSceneDocumentGroup)
                            progress = mSceneDocumentGroup.Progress().Ratio * 0.15f;
                        break;
                    case StartupState::MaterializeScene:
                    {
                        stage = "Preparing scene...";
                        const SceneMaterializationProgress sceneProgress =
                            mSceneMaterializer.GetProgress();
                        progress = 0.15f + sceneProgress.Ratio * 0.55f;
                        break;
                    }
                    case StartupState::PrepareRuntimeResources:
                    {
                        stage = "Warming up lighting...";
                        progress = 0.85f;
                        break;
                    }
                    case StartupState::StartRuntime:
                    case StartupState::PublishScene:
                    case StartupState::AwaitGameplayPresent:
                    case StartupState::StartAgent:
                    case StartupState::Complete:
                        stage = "Finalizing gameplay...";
                        progress = 0.98f;
                        break;
                    case StartupState::Detached:
                    case StartupState::Ready:
                        break;
                }

                progress = std::clamp(progress, 0.0f, 1.0f);
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
#endif
        }

        ImGui::End();
        ImGui::PopStyleVar(3);

        if (mCurrentScene)
        {
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const float uiScale = Application::GetInstance().GetImGuiLayer() ? Application::GetInstance().GetImGuiLayer()->GetUIScale() : 1.0f;
            UIViewportContext::Set(vp->WorkPos.x, vp->WorkPos.y, vp->WorkSize.x, vp->WorkSize.y, uiScale);
            mCurrentScene->OnImGuiRender();
        }
    }

    void GameLayer::OnEvent(Event& e)
    {
        if (mStartupState == StartupState::Ready && mCurrentScene)
            RmlUiManager::GetInstance().OnEvent(e);
    }

    void GameLayer::UpdateRuntimeCursorPolicy()
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

    void GameLayer::SetRuntimeCursorCaptured(bool bCaptured)
    {
        if (mRuntimeCursorCaptured == bCaptured)
            return;

        if (bCaptured)
            CenterCursorToWindow();

        Application::GetWindow().SetMouseCursorCaptured(bCaptured);

        mRuntimeCursorCaptured = bCaptured;
    }

    void GameLayer::CenterCursorToWindow()
    {
        Application::GetWindow().CenterMouseCursor();
    }
}
