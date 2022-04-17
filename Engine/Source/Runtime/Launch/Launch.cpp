#include "hepch.h"
#include "Runtime/Core/Application.h"

namespace HEngine
{
	// To be defined in CLIENT
	extern void MyAppInitialize(Application& app);
}

int main(int argc, char** argv)
{
	HEngine::MyAppInitialize(HEngine::Application::GetInstance());
	HEngine::Application::GetInstance().Run();
	HEngine::Application::GetInstance().Clean();
}