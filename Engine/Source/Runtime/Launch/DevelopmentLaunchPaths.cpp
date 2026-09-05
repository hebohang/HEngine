#include "Hepch.h"

#include "Runtime/Launch/DevelopmentLaunchPaths.h"

namespace HEngine
{
	namespace
	{
		[[nodiscard]] std::filesystem::path ResolveConfiguredRoot(
			const std::filesystem::path& configuredRoot,
			const std::filesystem::path& executableDirectory)
		{
			if (configuredRoot.is_absolute())
				return configuredRoot.lexically_normal();
			return (executableDirectory / configuredRoot).lexically_normal();
		}
	}

	DevelopmentLaunchPaths ResolveDevelopmentLaunchPaths(
		const std::filesystem::path& requestedProject,
		const std::filesystem::path& executableDirectory,
		const std::filesystem::path& configuredProjectRoot,
		const std::filesystem::path& configuredEngineContentRoot,
		const std::filesystem::path& configuredEngineGeneratedContentRoot)
	{
		DevelopmentLaunchPaths paths;
		paths.ProjectSelection = requestedProject.empty()
			? ResolveConfiguredRoot(configuredProjectRoot, executableDirectory)
			: requestedProject;
		paths.EngineContentRoot = ResolveConfiguredRoot(
			configuredEngineContentRoot, executableDirectory);
		paths.EngineGeneratedContentRoot = ResolveConfiguredRoot(
			configuredEngineGeneratedContentRoot, executableDirectory);
		return paths;
	}
}
