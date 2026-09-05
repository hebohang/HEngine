#include "Hepch.h"

#include "Runtime/UnitTest/RuntimeSceneRequestTests.h"

#include "Runtime/Core/AppFramework/Application.h"

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
		Application& application = Application::GetInstance();
		application.ClearRuntimeSceneRequest();

		Require(!application.RequestRuntimeScene({}),
			"An empty runtime scene request was accepted.");
		Require(!application.HasRuntimeSceneRequest(),
			"Rejecting an empty scene request changed the request slot.");

		Require(application.RequestRuntimeScene("Assets/Scenes/Physics3DTest.he"),
			"The first runtime scene request was rejected.");
		Require(application.HasRuntimeSceneRequest(),
			"The accepted runtime scene request was not retained.");
		Require(!application.RequestRuntimeScene("Assets/CornellBox/CornellBox.he"),
			"A pending runtime scene request was overwritten.");

		const auto first = application.ConsumeRuntimeSceneRequest();
		Require(first && *first == "Assets/Scenes/Physics3DTest.he",
			"The runtime scene request was not consumed exactly.");
		Require(!application.HasRuntimeSceneRequest() &&
			!application.ConsumeRuntimeSceneRequest(),
			"A consumed runtime scene request remained observable.");

		Require(application.RequestRuntimeScene("Assets/CornellBox/CornellBox.he"),
			"The request slot was not reusable after consume.");
		application.ClearRuntimeSceneRequest();
		application.ClearRuntimeSceneRequest();
		Require(!application.HasRuntimeSceneRequest(),
			"Clearing the runtime scene request was not idempotent.");
	}
}
