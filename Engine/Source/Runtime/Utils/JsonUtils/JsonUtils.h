#pragma once

#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Utils/FileUtils/AtomicFile.h"

#include <cstddef>
#include <filesystem>
#include <stop_token>

namespace HEngine
{
	namespace Utils
	{
		struct JsonLoadLimits
		{
			size_t MaxFileBytes = 32u * 1024u * 1024u;
			size_t MaxDepth = 64;
			size_t MaxNodes = 1'000'000;
		};

		struct JsonLoadResult
		{
			Json Value;
			size_t SourceBytes = 0;
		};

		Json LoadJsonFile(const std::filesystem::path& jsonFilePath);
		JsonLoadResult LoadJsonFile(
			const std::filesystem::path& jsonFilePath,
			const JsonLoadLimits& limits,
			std::stop_token stopToken = {});

		// Throws on serialization, open, write, flush, close, or atomic publish failure.
		// The previous destination is preserved when an exception is reported.
		void SaveJsonToFile(const Json& json, const std::filesystem::path& jsonFilePath);
		void SaveJsonToFile(
			const Json& json,
			const std::filesystem::path& jsonFilePath,
			AtomicWriteMode writeMode);
	}
}
