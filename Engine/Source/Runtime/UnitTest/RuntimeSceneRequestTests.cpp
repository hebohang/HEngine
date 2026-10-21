#include "Hepch.h"

#include "Runtime/UnitTest/RuntimeSceneRequestTests.h"

#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Graphics/Scene/RenderResourceWarmup.h"

#include <stdexcept>
#include <string>

namespace HEngine::Tests
{
	namespace
	{
		void Require(bool condition, const char* message)
		{
			if (!condition)
				throw std::runtime_error(message);
		}
	}

	void RuntimeSceneRequestContract()
	{
        RenderResourceWarmup warmup;
        warmup.Begin(10.0, 20);
        Require(warmup.Observe(true, 20, 10.1) == RenderResourceWarmupState::Pending,
            "A pre-warmup renderer snapshot completed scene warmup.");
        Require(warmup.Observe(false, 21, 10.2) == RenderResourceWarmupState::Pending,
            "Pending GPU resources completed scene warmup.");
        Require(warmup.Observe(true, 22, 10.3) == RenderResourceWarmupState::Pending &&
                warmup.Observe(true, 22, 10.4) == RenderResourceWarmupState::Pending,
            "Repeated polling of the same rendered frame completed warmup.");
        Require(warmup.Observe(true, 23, 10.5) == RenderResourceWarmupState::Pending,
            "Scene warmup skipped its stable-frame requirement.");
        Require(warmup.Observe(false, 24, 10.6) == RenderResourceWarmupState::Pending,
            "New resource requests did not interrupt stable warmup.");
        Require(warmup.Observe(true, 25, 10.7) == RenderResourceWarmupState::Pending &&
                warmup.Observe(true, 26, 10.8) == RenderResourceWarmupState::Pending &&
                warmup.Observe(true, 27, 10.9) == RenderResourceWarmupState::Ready,
            "Completed uploads and distinct stable frames failed to complete warmup.");
        warmup.Begin(20.0, 27);
        Require(warmup.Observe(false, 1, 21.0) == RenderResourceWarmupState::Pending &&
                warmup.Observe(true, 2, 21.1) == RenderResourceWarmupState::Pending,
            "Replacing a scene retained a previous scene's warmup readiness.");
        Require(warmup.Observe(false, 3, 110.0) == RenderResourceWarmupState::TimedOut,
            "A stalled texture upload left scene warmup waiting indefinitely.");

		Application& application = Application::GetInstance();
		application.ClearRuntimeSceneRequest();

		Require(!application.RequestRuntimeScene({}),
			"An empty runtime scene request was accepted.");
		Require(!application.HasRuntimeSceneRequest(),
			"Rejecting an empty scene request changed the request slot.");

		Require(application.RequestRuntimeScene("Assets/Scenes/MainMenu.he"),
			"The first runtime scene request was rejected.");
		Require(application.HasRuntimeSceneRequest(),
			"The accepted runtime scene request was not retained.");
		Require(!application.RequestRuntimeScene("Assets/Scenes/MainGame.he"),
			"A pending runtime scene request was overwritten.");

		const auto first = application.ConsumeRuntimeSceneRequest();
		Require(first && *first == "Assets/Scenes/MainMenu.he",
			"The runtime scene request was not consumed exactly.");
		Require(!application.HasRuntimeSceneRequest() &&
			!application.ConsumeRuntimeSceneRequest(),
			"A consumed runtime scene request remained observable.");

		Require(application.RequestRuntimeScene("Assets/Scenes/MainGame.he"),
			"The request slot was not reusable after consume.");
		application.ClearRuntimeSceneRequest();
		application.ClearRuntimeSceneRequest();
		Require(!application.HasRuntimeSceneRequest(),
			"Clearing the runtime scene request was not idempotent.");

		Require(application.RequestRuntimeScene("Assets/Scenes/MainMenu.he", true),
			"A forced same-scene reload request was rejected.");
		bool forceReload = false;
		const auto reload = application.ConsumeRuntimeSceneRequest(&forceReload);
		Require(reload && *reload == "Assets/Scenes/MainMenu.he" && forceReload,
			"Forced reload metadata did not survive queue consumption.");
		Require(application.RequestRuntimeScene("Assets/Scenes/MainGame.he"),
			"A normal scene request was rejected after forced reload.");
		(void)application.ConsumeRuntimeSceneRequest(&forceReload);
		Require(!forceReload, "Forced reload state leaked into an ordinary scene request.");
		Require(application.RequestRuntimeScene("Assets/Scenes/MainGame.he", true),
			"Second forced reload could not use the request slot.");
		application.ClearRuntimeSceneRequest();
		(void)application.ConsumeRuntimeSceneRequest(&forceReload);
		Require(!forceReload, "Cleared forced reload state remained observable.");
	}
}
