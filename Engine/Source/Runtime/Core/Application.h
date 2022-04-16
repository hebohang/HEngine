#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Window.h"
#include "Runtime/Core/LayerStack.h"
#include "Runtime/Events/Event.h"
#include "Runtime/Events/ApplicationEvent.h"

#include "Runtime/Core/Timestep.h"

#include "Runtime/ImGui/ImGuiLayer.h"

int main(int argc, char** argv);

namespace HEngine
{
	class Application
	{
	public:
		Application(const std::string& name = "HEngine");
		virtual ~Application();

		void OnEvent(Event& e);

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* layer);
		void PopLayer(Layer* layer);

		Window& GetWindow() { return *m_Window; }

		void Close();

		ImGuiLayer* GetImGuiLayer() { return m_ImGuiLayer; }

		static Application& Get() { return *s_Instance; }
	private:
		void Init();
		void Run();
		bool OnWindowClose(WindowCloseEvent& e);
		bool OnWindowResize(WindowResizeEvent& e);
	private:
		Scope<Window> m_Window;
		ImGuiLayer* m_ImGuiLayer;
		bool m_Running = true;
		bool m_Minimized = false;
		LayerStack m_LayerStack;
		float m_LastFrameTime = 0.0f;
	private:
		static Application* s_Instance;
		friend int ::main(int argc, char** argv);
	};

	// To be defined in CLIENT
	Application* CreateApplication();
}



