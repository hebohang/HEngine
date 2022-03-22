#pragma once

#include "Core.h"
#include "Window.h"
#include "HEngine/LayerStack.h"
#include "HEngine/Events/Event.h"
#include "HEngine/Events/ApplicationEvent.h"

#include "HEngine/ImGui/ImGuiLayer.h"

#include "HEngine/Renderer/Shader.h"
#include "HEngine/Renderer/Buffer.h"
#include "HEngine/Renderer/VertexArray.h"

namespace HEngine
{
	class HENGINE_API Application
	{
	public:
		Application();
		virtual ~Application();

		void Run();

		void OnEvent(Event& e);

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* layer);

		inline Window& GetWindow() { return *m_Window; }
		inline static Application& Get() { return *s_Instance; }
	private:
		bool OnWindowClose(WindowCloseEvent& e);

		std::unique_ptr<Window> m_Window;
		ImGuiLayer* m_ImGuiLayer;
		bool m_Running = true;
		LayerStack m_LayerStack;

		std::shared_ptr<Shader> m_Shader;
		std::shared_ptr<VertexArray> m_VertexArray;

		std::shared_ptr<Shader> m_BlueShader;
		std::shared_ptr<VertexArray> m_SquareVA;
	private:
		static Application* s_Instance;
	};

	// To be defined in CLIENT
	Application* CreateApplication();
}



