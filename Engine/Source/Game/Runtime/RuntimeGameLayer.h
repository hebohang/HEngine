#pragma once

#include "HEngine.h"
#include "Runtime/EcsFramework/Serializer/SceneDocument.h"
#include "Runtime/EcsFramework/Serializer/SceneSerializer.h"
#include "Runtime/Graphics/Scene/RenderResourceWarmup.h"

#if defined(HE_DEV_AGENT_RUNTIME)
#include "Game/AgentRuntime/DevAgentRuntime.h"
#include "Game/Runtime/InputReplayController.h"
#endif

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace HEngine
{
    struct RenderResourceWarmupRequest;
    class RuntimeGameLayer : public Layer
    {
    public:
        RuntimeGameLayer();
        virtual ~RuntimeGameLayer() = default;

        virtual void OnAttach()      override;
        virtual void OnDetach()      override;
        void         OnPreUpdate()   override;
        void         OnUpdate(Timestep ts) override;
        virtual void OnImGuiRender() override;
        void         OnEvent(Event& e) override;
    private:
        enum class StartupState : std::uint8_t
        {
            Detached,
            AwaitFirstPresent,
            LoadGameConfig,
            WaitBootResources,
            QueueSceneDocument,
            WaitSceneDocument,
            MaterializeScene,
            PrepareRuntimeResources,
            StartRuntime,
            PublishScene,
            AwaitGameplayPresent,
            StartAgent,
            Complete,
            Ready
        };

		enum class RuntimeSceneTransitionState : std::uint8_t
		{
			Idle,
			WaitSceneDocument,
			MaterializeScene,
			PrepareRuntimeResources,
			Commit,
            WarmRenderResources
		};

        void AdvanceStartup();
        void BeginRenderResourceWarmup();
        [[nodiscard]] bool PollRenderResourceWarmup();
		void BeginRuntimeSceneTransition(std::string scenePath, bool bForceReload = false);
		void AdvanceRuntimeSceneTransition();
		void AbortRuntimeSceneTransition(const std::exception_ptr& failure) noexcept;
        void ResetStartupAndRuntime() noexcept;
        void UpdateRuntimeCursorPolicy();
        void SetRuntimeCursorCaptured(bool bCaptured);
        void CenterCursorToWindow();
        void RefreshLoadingScreenSettings();
        void GetLoadingScreenProgress(const char*& stage, float& progress) const;
        void UpdateLoadingScreen();

        StartupState mStartupState = StartupState::Detached;
        std::string mDefaultScene;
        SceneDocumentPath mSceneDocumentPath;
        RuntimeResourceGroup mSceneDocumentGroup;
        IncrementalSceneMaterializer mSceneMaterializer;
        Ref<Level> mPendingScene;
        Ref<Level> mCurrentScene;
        bool mPendingRuntimeStarted = false;
        RenderResourceWarmup mRenderResourceWarmup;
        std::vector<Ref<RenderResourceWarmupRequest>> mSceneMeshWarmups;
        uint32_t mWarmupPeakTextures = 0;
        uint32_t mWarmupLastMeshUploads = 0;
        uint64_t mWarmupRenderedFrames = 0;
        bool bWarmupHasRenderWorld = false;
		RuntimeSceneTransitionState mRuntimeSceneTransitionState =
			RuntimeSceneTransitionState::Idle;
		std::string mRuntimeSceneTransitionPath;
		SceneDocumentPath mRuntimeSceneDocumentPath;
		RuntimeResourceGroup mRuntimeSceneDocumentGroup;
		IncrementalSceneMaterializer mRuntimeSceneMaterializer;
		Ref<Level> mRuntimeSceneCandidate;
        bool mRuntimeCursorCaptured = false;
        bool mRuntimeAltDown = false;
        bool mCopperLoadingScreen = false;
        bool mLoadingScreenChinese = false;
        std::string mLoadingScreenTitle;
        std::string mLoadingScreenDocumentPath = "Assets/UI/Loading/CopperNoir.rml";
        std::string mLoadingScreenDocument;
        UUID mLoadingScreenOwner;
        bool mLoadingScreenVisible = false;
#if defined(HE_DEV_AGENT_RUNTIME)
        Scope<DevAgentRuntime> mAgentRuntime;
        Scope<InputReplayController> mInputReplay;
#endif
    };
}
