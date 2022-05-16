#include "hepch.h"
#include "Runtime/Core/AppFramework/Application.h"

#include <pybind11/embed.h>

namespace HEngine
{
	// To be defined in CLIENT
	extern void MyAppInitialize(Application& app);
}

int main(int argc, char** argv)
{
	// https://github.com/pybind/pybind11/issues/3112
	// we should only have one interpreter in our program
	pybind11::scoped_interpreter guard{};

	HEngine::MyAppInitialize(HEngine::Application::GetInstance());
	HEngine::Application::GetInstance().Run();
	HEngine::Application::GetInstance().Clean();
}