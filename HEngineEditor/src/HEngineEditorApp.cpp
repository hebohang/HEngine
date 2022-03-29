#include <HEngine.h>
#include <HEngine/Core/EntryPoint.h>	 

#include "Platform/OpenGL/OpenGLShader.h"

#include "imgui/imgui.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "EditorLayer.h"

namespace HEngine
{
	class HEngineEditor : public Application
	{
	public:
		HEngineEditor()
			: Application("HEngine Editor")
		{
			PushLayer(new EditorLayer());
		}

		~HEngineEditor()
		{
		}
	};

	Application* CreateApplication()
	{
		return new HEngineEditor();
	}
}
