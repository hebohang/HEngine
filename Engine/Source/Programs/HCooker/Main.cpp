#include "Cooker.h"

#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

namespace
{
	void PrintUsage()
	{
		std::printf(
			"Usage: HCooker --project <project-root> --output <output-directory> "
			"[--config <config-path>] [--report <report-path>] "
			"[--engine-config <engine-cook-config>] "
			"[--python-plugin enabled|disabled] "
			"[--csharp-plugin enabled|disabled] "
			"[--csharp-metadata <generated-metadata-json>] "
			"[--forbid-root <absolute-directory>]... [--run-id <identifier>]\n"
			"       HCooker --self-test\n");
	}
}

int main(int argc, char** argv)
{
	if (argc == 2 && std::string_view(argv[1]) == "--self-test")
	{
		try
		{
			return HEngine::Cooker::RunSelfTests();
		}
		catch (const std::exception& exception)
		{
			std::fprintf(stderr, "HCooker self-test failed: %s\n", exception.what());
			return 1;
		}
	}

	HEngine::Cooker::CookOptions options;
	bool bHasProject = false;
	bool bHasOutput = false;

	for (int index = 1; index < argc; ++index)
	{
		const std::string_view argument = argv[index];
		if (argument == "--help" || argument == "-h")
		{
			PrintUsage();
			return 0;
		}

		if (index + 1 >= argc)
		{
			std::fprintf(stderr, "HCooker: missing value for argument '%s'.\n", argv[index]);
			PrintUsage();
			return 2;
		}

		const std::string rawValue = argv[++index];
		const std::filesystem::path value = rawValue;
		if (argument == "--project")
		{
			options.ProjectRoot = value;
			bHasProject = true;
		}
		else if (argument == "--output")
		{
			options.OutputDirectory = value;
			bHasOutput = true;
		}
		else if (argument == "--config")
		{
			options.ConfigPath = value;
		}
		else if (argument == "--engine-config")
		{
			options.EngineConfigPath = value;
		}
		else if (argument == "--python-plugin" || argument == "--csharp-plugin")
		{
			if (rawValue != "enabled" && rawValue != "disabled")
			{
				std::fprintf(stderr, "HCooker: script plugin mode must be enabled or disabled.\n");
				return 2;
			}
			if (argument == "--python-plugin") options.PythonPluginEnabled = rawValue == "enabled";
			else options.CSharpPluginEnabled = rawValue == "enabled";
		}
		else if (argument == "--report")
		{
			options.ReportPath = value;
		}
		else if (argument == "--csharp-metadata")
		{
			options.CSharpMetadataPath = value;
		}
		else if (argument == "--forbid-root")
		{
			options.ForbiddenRoots.push_back(value);
		}
		else if (argument == "--run-id")
		{
			options.RunId = rawValue;
		}
		else
		{
			std::fprintf(stderr, "HCooker: unknown argument '%s'.\n", argv[index - 1]);
			PrintUsage();
			return 2;
		}
	}

	if (!bHasProject || !bHasOutput)
	{
		std::fprintf(stderr, "HCooker: --project and --output are required.\n");
		PrintUsage();
		return 2;
	}

	try
	{
		return HEngine::Cooker::Run(options);
	}
	catch (const std::exception& exception)
	{
		HEngine::Cooker::MarkRunFailed(options, exception.what());
		std::fprintf(stderr, "HCooker failed: %s\n", exception.what());
		return 1;
	}
}
