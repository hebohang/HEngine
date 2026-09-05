#include "Hepch.h"

#include "Runtime/EcsFramework/Serializer/SceneDocument.h"

#include "Runtime/Core/UUID.h"
#include "Runtime/Profile/Profile.h"
#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceCatalog.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace HEngine
{
	namespace
	{
		inline constexpr char TagComponentName[] = "TagComponent";
		inline constexpr char TagFieldName[] = "Tag";
		inline constexpr char MeshComponentName[] = "MeshComponent";
		inline constexpr char MeshPathFieldName[] = "Path";

		[[noreturn]] void ThrowSceneLoadCancelled(const SceneDocumentPath& path)
		{
			throw std::runtime_error("Scene document load was cancelled: " + Utils::PathToUtf8(path.FullPath));
		}

		void CheckSceneLoadCancelled(
			const SceneDocumentPath& path,
			std::stop_token stopToken)
		{
			if (stopToken.stop_requested())
				ThrowSceneLoadCancelled(path);
		}

		[[nodiscard]] size_t SaturatingSize(uint64_t value) noexcept
		{
			constexpr uint64_t maxSize = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
			return value > maxSize ? std::numeric_limits<size_t>::max() : static_cast<size_t>(value);
		}

		[[nodiscard]] size_t GetResidentBytes(
			const RuntimeResourceKey& key,
			const std::filesystem::path& fullPath)
		{
			const std::shared_ptr<const RuntimeResourceCatalogSnapshot> snapshot =
				RuntimeResourceCatalog::GetInstance().GetSnapshot();
			if (const RuntimeResourceCatalogEntry* entry = snapshot ? snapshot->Find(key.Path) : nullptr)
				return SaturatingSize(entry->CpuBytes);

			std::error_code error;
			const uintmax_t fileSize = std::filesystem::file_size(fullPath, error);
			return error ? 0 : SaturatingSize(static_cast<uint64_t>(fileSize));
		}

		[[nodiscard]] std::string ReadEntityTag(
			const std::string& uuid,
			const Json& entityValue,
			bool& outHasTag)
		{
			outHasTag = false;
			const auto tagComponent = entityValue.find(TagComponentName);
			if (tagComponent == entityValue.end() || !tagComponent->is_object())
				return {};

			const auto tag = tagComponent->find(TagFieldName);
			if (tag == tagComponent->end())
				return {};
			if (!tag->is_string())
			{
				throw std::runtime_error(
					"Scene entity TagComponent.Tag must be a string: " + uuid);
			}

			outHasTag = true;
			return tag->get<std::string>();
		}

		void ValidateEntityUuid(const std::string& uuid)
		{
			try
			{
				if (UUID(uuid).isValid())
					return;
			}
			catch (...)
			{
				std::throw_with_nested(
					std::runtime_error("Scene entity key is not a valid UUID: " + uuid));
			}

			throw std::runtime_error("Scene entity key is not a valid UUID: " + uuid);
		}

		void PrescanMeshPath(
			const SceneDocumentPath& scenePath,
			const SceneDocumentLimits& limits,
			const std::string& uuid,
			const Json& entityValue,
			std::unordered_set<std::string>& meshCacheKeys,
			std::vector<std::string>& meshPaths)
		{
			const auto meshComponent = entityValue.find(MeshComponentName);
			if (meshComponent == entityValue.end())
				return;
			if (!meshComponent->is_object())
				throw std::runtime_error("Scene entity MeshComponent must be an object: " + uuid);

			const auto meshPathValue = meshComponent->find(MeshPathFieldName);
			if (meshPathValue == meshComponent->end() || !meshPathValue->is_string())
				throw std::runtime_error("Scene entity MeshComponent.Path must be a string: " + uuid);

			const std::string projectPath = meshPathValue->get<std::string>();
			if (projectPath == "None")
				return;
			if (projectPath.size() > limits.MaxMeshPathBytes)
			{
				throw std::runtime_error(
					"Scene entity MeshComponent.Path exceeds the configured byte limit: " + uuid);
			}

			std::filesystem::path fullMeshPath;
			std::string normalizedMeshPath;
			const Utils::PathValidationResult pathResult = Utils::TryResolveProjectRelativePath(
				scenePath.ProjectRoot,
				projectPath,
				fullMeshPath,
				normalizedMeshPath);
			if (!pathResult)
			{
				throw std::runtime_error(
					"Scene entity MeshComponent.Path is invalid for " + uuid + ": " +
					pathResult.Message);
			}

			const RuntimeResourceKey meshKey = RuntimeResourceKey::FromProjectPath(
				RuntimeResourceKind::Mesh,
				normalizedMeshPath);
			if (!meshKey.CachePath.ends_with(".hmesh"))
				return;

			if (meshCacheKeys.contains(meshKey.CachePath))
				return;
			if (meshPaths.size() == limits.MaxUniqueMeshPaths)
			{
				throw std::runtime_error(
					"Scene document exceeds the configured unique mesh path limit: " +
					Utils::PathToUtf8(scenePath.FullPath));
			}

			meshCacheKeys.emplace(meshKey.CachePath);
			meshPaths.emplace_back(meshKey.Path);
		}
	}

	SceneDocument::SceneDocument(
		std::vector<SceneDocumentEntity> entities,
		std::vector<std::string> meshPaths,
		size_t sourceBytes)
		: mEntities(std::move(entities)),
		mMeshPaths(std::move(meshPaths)),
		mSourceBytes(sourceBytes)
	{
	}

	const std::vector<SceneDocumentEntity>& SceneDocument::GetEntities() const noexcept
	{
		return mEntities;
	}

	const std::vector<std::string>& SceneDocument::GetMeshPaths() const noexcept
	{
		return mMeshPaths;
	}

	size_t SceneDocument::GetSourceBytes() const noexcept
	{
		return mSourceBytes;
	}

	SceneDocumentPath ResolveSceneDocumentPath(
		const std::filesystem::path& projectRoot,
		std::string_view projectPath)
	{
		SceneDocumentPath path;
		std::string normalizedPath;
		const Utils::PathValidationResult result = Utils::TryResolveProjectRelativePath(
			projectRoot,
			projectPath,
			path.FullPath,
			normalizedPath);
		if (!result)
			throw std::invalid_argument(result.Message);

		path.Key = RuntimeResourceKey::FromProjectPath(RuntimeResourceKind::Scene, normalizedPath);
		if (!path.Key.CachePath.ends_with(".he"))
			throw std::invalid_argument("Scene document path must use the .he extension.");

		std::error_code error;
		const std::filesystem::path absoluteRoot = std::filesystem::absolute(projectRoot, error);
		if (error)
			throw std::invalid_argument("Cannot make the scene project root absolute: " + error.message());
		path.ProjectRoot = std::filesystem::weakly_canonical(absoluteRoot, error);
		if (error)
			throw std::invalid_argument("Cannot resolve the scene project root: " + error.message());

		path.ResidentBytes = GetResidentBytes(path.Key, path.FullPath);
		return path;
	}

	SceneDocument ParseSceneDocument(
		const SceneDocumentPath& path,
		const SceneDocumentLimits& limits,
		std::stop_token stopToken)
	{
		HE_PROFILE_SECTION("SceneSerializer::Deserialize");

		if (path.Key.Kind != RuntimeResourceKind::Scene || path.Key.bBuiltin ||
			path.ProjectRoot.empty() || path.FullPath.empty())
		{
			throw std::invalid_argument("Scene document path is not a resolved project scene path.");
		}

		CheckSceneLoadCancelled(path, stopToken);
		Utils::JsonLoadResult loaded = Utils::LoadJsonFile(
			path.FullPath,
			{
				limits.MaxFileBytes,
				limits.MaxJsonDepth,
				limits.MaxJsonNodes
			},
			stopToken);
		if (!loaded.Value.is_object())
			throw std::runtime_error("Scene document root must be an object: " + Utils::PathToUtf8(path.FullPath));
		if (loaded.Value.size() > limits.MaxEntities)
		{
			throw std::runtime_error(
				"Scene document exceeds the configured entity limit: " + Utils::PathToUtf8(path.FullPath));
		}

		std::vector<SceneDocumentEntity> entities;
		entities.reserve(loaded.Value.size());
		std::vector<std::string> meshPaths;
		meshPaths.reserve(std::min(loaded.Value.size(), limits.MaxUniqueMeshPaths));
		std::unordered_set<std::string> meshCacheKeys;
		meshCacheKeys.reserve(std::min(loaded.Value.size(), limits.MaxUniqueMeshPaths));

		for (auto& [uuid, entityValue] : loaded.Value.items())
		{
			CheckSceneLoadCancelled(path, stopToken);
			if (!entityValue.is_object())
				throw std::runtime_error("Scene entity must be an object: " + uuid);
			if (entityValue.size() > limits.MaxComponentsPerEntity)
			{
				throw std::runtime_error(
					"Scene entity exceeds the configured component limit: " + uuid);
			}

			bool bHasTag = false;
			std::string tag = ReadEntityTag(uuid, entityValue, bHasTag);
			if (!bHasTag)
				continue;

			ValidateEntityUuid(uuid);
			PrescanMeshPath(
				path,
				limits,
				uuid,
				entityValue,
				meshCacheKeys,
				meshPaths);
			entities.push_back({ uuid, std::move(tag), std::move(entityValue) });
		}

		CheckSceneLoadCancelled(path, stopToken);
		return SceneDocument(std::move(entities), std::move(meshPaths), loaded.SourceBytes);
	}

	RuntimeResourceHandle<SceneDocument> AcquireSceneDocument(
		RuntimeResourceRegistry& registry,
		const SceneDocumentPath& path,
		const SceneDocumentLimits& limits)
	{
		return registry.AcquireBlocking<SceneDocument>(
			path.Key,
			[path, limits]
			{
				return std::make_shared<SceneDocument>(ParseSceneDocument(path, limits));
			},
			path.ResidentBytes);
	}

	bool QueueSceneDocument(
		RuntimeResourceRegistry& registry,
		const SceneDocumentPath& path,
		RuntimeResourceGroup& group,
		const SceneDocumentLimits& limits)
	{
		if (!group)
			throw std::logic_error("Scene document queue requires a valid resource group.");

		return registry.QueueAsyncPinned<SceneDocument, SceneDocument>(
			path.Key,
			group,
			[path, limits](std::stop_token stopToken)
			{
				return ParseSceneDocument(path, limits, stopToken);
			},
			[](SceneDocument&& document)
			{
				return std::make_shared<SceneDocument>(std::move(document));
			},
			path.ResidentBytes);
	}
}
