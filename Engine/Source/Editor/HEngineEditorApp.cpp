#include "EditorLayer.h"

#include <HEngine.h>
#include <Runtime/Core/EntryPoint.h>	 

#include <imgui/imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

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
