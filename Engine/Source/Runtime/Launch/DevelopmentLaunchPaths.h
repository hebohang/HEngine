#pragma once

#include <filesystem>

namespace HEngine
{
	struct DevelopmentLaunchPaths
	{
		std::filesystem::path ProjectSelection;
		std::filesystem::path EngineContentRoot;
		std::filesystem::path EngineGeneratedContentRoot;
	};

	[[nodiscard]] DevelopmentLaunchPaths ResolveDevelopmentLaunchPaths(
		const std::filesystem::path& requestedProject,
		const std::filesystem::path& executableDirectory,
		const std::filesystem::path& configuredProjectRoot,
		const std::filesystem::path& configuredEngineContentRoot,
		const std::filesystem::path& configuredEngineGeneratedContentRoot);
}
