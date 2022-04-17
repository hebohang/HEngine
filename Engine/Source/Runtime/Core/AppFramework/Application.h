#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Core/Base/PublicSingleton.h"
#include "Runtime/Core/Window.h"
#include "Runtime/Core/AppFramework/Layer/LayerStack.h"
#include "Runtime/Events/Event.h"
#include "Runtime/Events/ApplicationEvent.h"

#include "Runtime/Core/Timestep.h"

#include "Runtime/ImGui/ImGuiLayer.h"

int main(int argc, char** argv);

namespace HEngine
{
	class Application : public PublicSingleton<Application>
	{
	public:
		Application() = default;
		virtual ~Application() {}

		void OnEvent(Event& e);

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* layer);
		void PopLayer(Layer* layer);

		Window& GetWindow() { return *m_Window; }

		void Close();

		ImGuiLayer* GetImGuiLayer() { return m_ImGuiLayer; }
	private:
		void Init(const std::string& name);
		void Run();
		void Clean();
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
		friend int ::main(int argc, char** argv);

		// To be defined in CLIENT
		friend void MyAppInitialize(Application& app);
	};
}



