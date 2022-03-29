#pragma once

#include "Base.h"
#include "Window.h"
#include "HEngine/Core/LayerStack.h"
#include "HEngine/Events/Event.h"
#include "HEngine/Events/ApplicationEvent.h"

#include "HEngine/Core/Timestep.h"

#include "HEngine/ImGui/ImGuiLayer.h"

namespace HEngine
{
	class Application
	{
	public:
		Application(const std::string& name = "HEngine");
		virtual ~Application();

		void Run();

		void OnEvent(Event& e);

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* layer);

		Window& GetWindow() { return *m_Window; }

		void Close();

		static Application& Get() { return *s_Instance; }
	private:
		bool OnWindowClose(WindowCloseEvent& e);
		bool OnWindowResize(WindowResizeEvent& e);
	private:
		std::unique_ptr<Window> m_Window;
		ImGuiLayer* m_ImGuiLayer;
		bool m_Running = true;
		bool m_Minimized = false;
		LayerStack m_LayerStack;
		float m_LastFrameTime = 0.0f;
	private:
		static Application* s_Instance;
	};

	// To be defined in CLIENT
	Application* CreateApplication();
}



