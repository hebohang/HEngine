#include "hepch.h"
#include "Application.h"

#include "HEngine/Events/ApplicationEvent.h"
#include "HEngine/Log.h"

namespace HEngine
{
	Application::Application()
	{
	}

	Application::~Application()
	{
	}

	void Application::Run()
	{
		WindowResizeEvent e(1280, 720);
		if (e.IsInCategory(EventCategoryApplication))
		{
			HE_TRACE(e);
		}
		if (e.IsInCategory(EventCategoryInput))
		{
			HE_TRACE(e);
		}

		while (true);
	}
}


