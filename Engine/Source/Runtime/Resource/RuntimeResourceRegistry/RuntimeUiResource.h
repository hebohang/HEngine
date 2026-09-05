#pragma once

#include "Runtime/Resource/ResType/UIRes.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeResourceRegistry.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace HEngine
{
	class RuntimeUiDefinition final
	{
	public:
		explicit RuntimeUiDefinition(UIRes definition);

		[[nodiscard]] const UIRes& GetDefinition() const noexcept;

	private:
		UIRes mDefinition;
	};

	struct RuntimeUiPath
	{
		RuntimeResourceKey Key;
		std::filesystem::path HuiFullPath;
		std::filesystem::path RmlFullPath;
		std::filesystem::path RcssFullPath;
		std::string HuiProjectPath;
		std::string RmlProjectPath;
		std::string RcssProjectPath;
		std::size_t ResidentBytes = 0;
	};

	[[nodiscard]] RuntimeUiPath ResolveRuntimeUiPath(
		const std::filesystem::path& projectRoot,
		std::string_view projectPath);
	[[nodiscard]] RuntimeUiPath ResolveRuntimeUiWrittenPath(
		const std::filesystem::path& projectRoot,
		const std::filesystem::path& writtenPath);

	[[nodiscard]] RuntimeResourceHandle<RuntimeUiDefinition> AcquireRuntimeUiDefinition(
		RuntimeResourceRegistry& registry,
		const RuntimeUiPath& path);
	[[nodiscard]] bool QueueRuntimeUiDefinition(
		RuntimeResourceRegistry& registry,
		const RuntimeUiPath& path,
		RuntimeResourceGroup& group);
	[[nodiscard]] bool SaveRuntimeUiDefinition(
		RuntimeResourceRegistry& registry,
		const RuntimeUiPath& path,
		const UIRes& definition,
		std::string* outError = nullptr);
}
