#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace HEngine::Cooker
{
	struct CookOptions
	{
		std::filesystem::path ProjectRoot;
		std::filesystem::path OutputDirectory;
		std::filesystem::path ConfigPath = "Config/Game.json";
		std::filesystem::path EngineConfigPath;
		std::filesystem::path ReportPath;
		std::string RunId;
		std::vector<std::filesystem::path> ForbiddenRoots;
	};

	int Run(const CookOptions& options);
	void MarkRunFailed(const CookOptions& options, const std::string& error) noexcept;
	int RunSelfTests();
}
