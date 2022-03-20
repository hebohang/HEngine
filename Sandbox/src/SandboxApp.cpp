#include <HEngine.h>

class ExampleLayer : public HEngine::Layer
{
public:
	ExampleLayer()
		: Layer("Example")
	{
	}

	void OnUpdate() override
	{
		HE_INFO("ExampleLayer::Update");
	}

	void OnEvent(HEngine::Event& event) override
	{
		HE_TRACE("{0}", event);
	}

};

class Sandbox : public HEngine::Application
{
public:
	Sandbox()
	{
		PushLayer(new ExampleLayer());
		PushOverlay(new HEngine::ImGuiLayer());
	}

	~Sandbox()
	{

	}
};

HEngine::Application* HEngine::CreateApplication()
{
	return new Sandbox();
}