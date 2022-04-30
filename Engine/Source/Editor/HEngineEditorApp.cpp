#include "EditorLayer.h"

#include <HEngine.h>

#include <imgui/imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace HEngine
{
	void MyAppInitialize(Application& app)
	{
		app.Init("HEngine Editor");
		//app.PushLayer(new EditorLayer());
	}
}
