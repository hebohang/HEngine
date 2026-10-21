#include "Hepch.h"
#include "JsonUtils.h"

namespace HEngine::Utils
{
	void SaveJsonToFile(const Json& json, const std::filesystem::path& jsonFilePath)
	{
		SaveJsonToFile(json, jsonFilePath, AtomicWriteMode::ReplaceExisting);
	}

	void SaveJsonToFile(
		const Json& json,
		const std::filesystem::path& jsonFilePath,
		AtomicWriteMode writeMode)
	{
		const std::string contents = json.dump(4);
		WriteTextToFileAtomically(jsonFilePath, contents, writeMode);
	}
}
