#pragma once

#ifdef HE_PLATFORM_WINDOWS

extern HEngine::Application* HEngine::CreateApplication();

int main(int argc, char** argv)
{
	HEngine::Log::Init();
	HE_CORE_WARN("Initialized Log!");
	int a = 5;
	HE_INFO("Hello! Var={0}", a);

	auto app = HEngine::CreateApplication();
	app->Run();
	delete app;
}

#endif