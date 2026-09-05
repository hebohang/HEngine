#include "Hepch.h"

#include "Runtime/UnitTest/DevelopmentLaunchPathsTests.h"

#include "Runtime/Launch/DevelopmentLaunchPaths.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace HEngine::Tests
{
	namespace
	{
		void Require(bool condition, std::string_view message)
		{
			if (!condition)
				throw std::runtime_error(std::string(message));
		}
	}

	void DevelopmentLaunchPathsContract()
	{
		const std::filesystem::path executableDirectory =
			"C:/Portable/HEngineEditor-1.0.0-Windows-x64";
		const DevelopmentLaunchPaths portable = ResolveDevelopmentLaunchPaths(
			{}, executableDirectory, "Project", "Engine/Content", "Engine/Content");
		Require(portable.ProjectSelection == executableDirectory / "Project" &&
				portable.EngineContentRoot == executableDirectory / "Engine/Content" &&
				portable.EngineGeneratedContentRoot == executableDirectory / "Engine/Content",
			"Portable Editor roots were not resolved relative to the executable.");

		const std::filesystem::path buildProject = "C:/Build/Project";
		const std::filesystem::path buildEngineContent = "C:/Build/Engine/Content";
		const std::filesystem::path buildGeneratedContent =
			"C:/Build/GeneratedContent/Engine";
		const DevelopmentLaunchPaths development = ResolveDevelopmentLaunchPaths(
			{}, executableDirectory, buildProject, buildEngineContent,
			buildGeneratedContent);
		Require(development.ProjectSelection == buildProject &&
				development.EngineContentRoot == buildEngineContent &&
				development.EngineGeneratedContentRoot == buildGeneratedContent,
			"A development build changed its configured absolute roots.");

		const DevelopmentLaunchPaths explicitProject = ResolveDevelopmentLaunchPaths(
			"ExternalProject", executableDirectory, "Project",
			"Engine/Content", "Engine/Content");
		Require(explicitProject.ProjectSelection == "ExternalProject" &&
				explicitProject.EngineContentRoot == executableDirectory / "Engine/Content",
			"An explicit project did not override only the configured project default.");
	}
}
