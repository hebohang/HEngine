#include <HEngine.h>

class Sandbox : public HEngine::Application
{
public:
	Sandbox()
	{

	}

	~Sandbox()
	{

	}
};

HEngine::Application* HEngine::CreateApplication()
{
	return new Sandbox();
}