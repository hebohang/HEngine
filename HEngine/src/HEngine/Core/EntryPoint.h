#pragma once

#ifdef HE_PLATFORM_WINDOWS

extern HEngine::Application* HEngine::CreateApplication();

int main(int argc, char** argv)
{
	HEngine::Log::Init();

	auto app = HEngine::CreateApplication();
	app->Init();

	app->Run();

	delete app;
}

#endif
