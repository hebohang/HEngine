#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeResourceRegistry.h"

#include <filesystem>
#include <memory>
#include <string_view>

namespace HEngine
{
	class Mesh;
	class RuntimeResourceCatalogSnapshot;

	namespace MeshRuntimeResource
	{
		[[nodiscard]] RuntimeResourceKey MakeKey(
			const std::filesystem::path& projectRoot,
			std::string_view projectPath);
		[[nodiscard]] bool QueueAsync(
			RuntimeResourceRegistry& registry,
			const std::filesystem::path& projectRoot,
			const std::shared_ptr<const RuntimeResourceCatalogSnapshot>& catalog,
			std::string_view projectPath,
			RuntimeResourceGroup& group);
		[[nodiscard]] Ref<Mesh> AcquirePrototype(
			RuntimeResourceRegistry& registry,
			const std::filesystem::path& projectRoot,
			const std::shared_ptr<const RuntimeResourceCatalogSnapshot>& catalog,
			std::string_view projectPath,
			RuntimeResourceGroup& group);

		// Compatibility helpers for legacy callers that use the process-wide
		// runtime registry, project configuration, and installed catalog.
		[[nodiscard]] RuntimeResourceKey MakeKey(std::string_view projectPath);
		[[nodiscard]] bool QueueAsync(
			std::string_view projectPath,
			RuntimeResourceGroup& group);
		[[nodiscard]] Ref<Mesh> AcquirePrototype(
			std::string_view projectPath,
			RuntimeResourceGroup& group);
	}
}
