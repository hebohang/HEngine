#pragma once

#include "HEngine.h"
#include "Runtime/EcsFramework/Serializer/SceneDocument.h"
#include "Runtime/EcsFramework/Serializer/SceneSerializer.h"

#if defined(HE_DEV_AGENT_RUNTIME)
#include "Game/AgentRuntime/DevAgentRuntime.h"
#endif

#include <cstdint>
#include <exception>
#include <string>

namespace HEngine
{
    class GameLayer : public Layer
    {
    public:
        GameLayer();
        virtual ~GameLayer() = default;

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
			Commit
		};

        void AdvanceStartup();
		void BeginRuntimeSceneTransition(std::string scenePath);
		void AdvanceRuntimeSceneTransition();
		void AbortRuntimeSceneTransition(const std::exception_ptr& failure) noexcept;
        void ResetStartupAndRuntime() noexcept;
        void UpdateRuntimeCursorPolicy();
        void SetRuntimeCursorCaptured(bool bCaptured);
        void CenterCursorToWindow();

        StartupState mStartupState = StartupState::Detached;
        std::string mDefaultScene;
        SceneDocumentPath mSceneDocumentPath;
        RuntimeResourceGroup mSceneDocumentGroup;
        IncrementalSceneMaterializer mSceneMaterializer;
        Ref<Level> mPendingScene;
        Ref<Level> mCurrentScene;
        bool mPendingRuntimeStarted = false;
		RuntimeSceneTransitionState mRuntimeSceneTransitionState =
			RuntimeSceneTransitionState::Idle;
		std::string mRuntimeSceneTransitionPath;
		SceneDocumentPath mRuntimeSceneDocumentPath;
		RuntimeResourceGroup mRuntimeSceneDocumentGroup;
		IncrementalSceneMaterializer mRuntimeSceneMaterializer;
		Ref<Level> mRuntimeSceneCandidate;
        bool mRuntimeCursorCaptured = false;
        bool mRuntimeAltDown = false;
#if defined(HE_DEV_AGENT_RUNTIME)
        Scope<DevAgentRuntime> mAgentRuntime;
#endif
    };
}
