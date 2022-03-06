#pragma once

#ifdef HE_PLATFORM_WINDOWS

extern HEngine::Application* HEngine::CreateApplication();

int main(int argc, char** argv)
{
	printf("HEngine!\n");
	auto app = HEngine::CreateApplication();
	app->Run();
	delete app;
}

#endif