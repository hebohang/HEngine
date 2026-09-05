#ifndef _DISABLE_MAIN

#include "TestHelper.h"
#include "test.h"
#include <iostream>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(void, llgi_report_test_result, (int result, const char* message), {
	let text = UTF8ToString(message);
	let status = result === 0 ? 'passed' : 'failed';
	if (status === 'passed' && Module.llgiLastWebGPUError) {
		status = 'failed';
		text = Module.llgiLastWebGPUError;
	}
	Module.llgiTestResult = { status: status, message: text };
	console.log(status === 'passed' ? 'LLGI_TEST_PASS' : 'LLGI_TEST_FAIL', text);
});
#endif

#ifdef _WIN32
#pragma comment(lib, "d3dcompiler.lib")

#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#include <stdlib.h>

#endif

#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(__EMSCRIPTEN__)

int main(int argc, char* argv[])
{

#if _WIN32
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

	auto args = TestHelper::ParseArg(argc, argv);

	// make shaders folder path from __FILE__
	{

#if defined(__EMSCRIPTEN__)
		TestHelper::SetRoot("/Shaders/WebGPU/");
#else
		auto path = std::string(__FILE__);
#if defined(WIN32)
		auto pos = path.find_last_of("\\");
#else
		auto pos = path.find_last_of("/");
#endif

		path = path.substr(0, pos);

		if (args.Device == LLGI::DeviceType::DirectX12)
		{
			TestHelper::SetRoot((path + "/Shaders/HLSL_DX12/").c_str());
		}
		else if (args.Device == LLGI::DeviceType::Metal)
		{
			TestHelper::SetRoot((path + "/Shaders/Metal/").c_str());
		}
		else if (args.Device == LLGI::DeviceType::Vulkan)
		{
#ifdef ENABLE_VULKAN_COMPILER
			TestHelper::SetRoot((path + "/Shaders/GLSL_VULKAN/").c_str());
#else
			TestHelper::SetRoot((path + "/Shaders/SPIRV/").c_str());
#endif
		}
		else if (args.Device == LLGI::DeviceType::WebGPU)
		{
			TestHelper::SetRoot((path + "/Shaders/WebGPU/").c_str());
		}
#endif
	}

	LLGI::SetLogger([](LLGI::LogType logType, const std::string& message) { std::cerr << message << std::endl; });

	TestHelper::SetIsCaptureRequired(true);

	TestHelper::Run(args);

	LLGI::SetLogger(nullptr);

	TestHelper::Dispose();

#ifdef __EMSCRIPTEN__
	llgi_report_test_result(0, "completed");
#endif

	return 0;
}
#endif

#endif
