#pragma once

#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeResourceRegistry.h"

#include <filesystem>
#include <string_view>

namespace HEngine
{
	class RuntimePrefabJson final
	{
	public:
		explicit RuntimePrefabJson(Json document);

		[[nodiscard]] const Json& GetDocument() const noexcept;

	private:
		Json mDocument;
	};

	struct RuntimePrefabPath
	{
		RuntimeResourceKey Key;
		std::filesystem::path FullPath;
		size_t ResidentBytes = 0;
	};

	[[nodiscard]] RuntimePrefabPath ResolveRuntimePrefabPath(
		const std::filesystem::path& projectRoot,
		std::string_view projectPath);
	[[nodiscard]] RuntimePrefabPath ResolveRuntimePrefabWrittenPath(
		const std::filesystem::path& projectRoot,
		const std::filesystem::path& writtenPath);

	[[nodiscard]] RuntimeResourceHandle<RuntimePrefabJson> AcquireRuntimePrefabJson(
		RuntimeResourceRegistry& registry,
		const RuntimePrefabPath& path);
	[[nodiscard]] bool QueueRuntimePrefabJson(
		RuntimeResourceRegistry& registry,
		const RuntimePrefabPath& path,
		RuntimeResourceGroup& group);
}
