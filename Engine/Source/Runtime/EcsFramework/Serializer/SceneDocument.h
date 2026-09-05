#pragma once

#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeResourceRegistry.h"

#include <cstddef>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
	struct SceneDocumentLimits
	{
		size_t MaxFileBytes = 32u * 1024u * 1024u;
		size_t MaxEntities = 65'536;
		size_t MaxComponentsPerEntity = 64;
		size_t MaxJsonDepth = 64;
		size_t MaxJsonNodes = 1'000'000;
		size_t MaxUniqueMeshPaths = 4'096;
		size_t MaxMeshPathBytes = 1'024;
	};

	struct SceneDocumentEntity
	{
		std::string Uuid;
		std::string Tag;
		Json Value;
	};

	class SceneDocument final
	{
	public:
		SceneDocument(
			std::vector<SceneDocumentEntity> entities,
			std::vector<std::string> meshPaths,
			size_t sourceBytes);

		[[nodiscard]] const std::vector<SceneDocumentEntity>& GetEntities() const noexcept;
		[[nodiscard]] const std::vector<std::string>& GetMeshPaths() const noexcept;
		[[nodiscard]] size_t GetSourceBytes() const noexcept;

	private:
		std::vector<SceneDocumentEntity> mEntities;
		std::vector<std::string> mMeshPaths;
		size_t mSourceBytes = 0;
	};

	struct SceneDocumentPath
	{
		RuntimeResourceKey Key;
		std::filesystem::path ProjectRoot;
		std::filesystem::path FullPath;
		size_t ResidentBytes = 0;
	};

	[[nodiscard]] SceneDocumentPath ResolveSceneDocumentPath(
		const std::filesystem::path& projectRoot,
		std::string_view projectPath);
	[[nodiscard]] SceneDocument ParseSceneDocument(
		const SceneDocumentPath& path,
		const SceneDocumentLimits& limits = {},
		std::stop_token stopToken = {});
	[[nodiscard]] RuntimeResourceHandle<SceneDocument> AcquireSceneDocument(
		RuntimeResourceRegistry& registry,
		const SceneDocumentPath& path,
		const SceneDocumentLimits& limits = {});
	[[nodiscard]] bool QueueSceneDocument(
		RuntimeResourceRegistry& registry,
		const SceneDocumentPath& path,
		RuntimeResourceGroup& group,
		const SceneDocumentLimits& limits = {});
}
