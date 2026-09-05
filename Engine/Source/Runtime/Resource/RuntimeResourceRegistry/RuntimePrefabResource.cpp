#include "Hepch.h"

#include "Runtime/Resource/RuntimeResourceRegistry/RuntimePrefabResource.h"

#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceCatalog.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <limits>
#include <stdexcept>
#include <string>

namespace HEngine
{
	namespace
	{
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

		[[nodiscard]] bool IsInsideRoot(
			const std::filesystem::path& canonicalRoot,
			const std::filesystem::path& candidate)
		{
			const std::filesystem::path relative = candidate.lexically_relative(canonicalRoot);
			if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
				return false;
			for (const std::filesystem::path& component : relative)
			{
				if (component == "..")
					return false;
			}
			return true;
		}
	}

	RuntimePrefabJson::RuntimePrefabJson(Json document)
		: mDocument(std::move(document))
	{
	}

	const Json& RuntimePrefabJson::GetDocument() const noexcept
	{
		return mDocument;
	}

	RuntimePrefabPath ResolveRuntimePrefabPath(
		const std::filesystem::path& projectRoot,
		std::string_view projectPath)
	{
		RuntimePrefabPath path;
		std::string normalizedPath;
		const Utils::PathValidationResult result = Utils::TryResolveProjectRelativePath(
			projectRoot,
			projectPath,
			path.FullPath,
			normalizedPath);
		if (!result)
			throw std::invalid_argument(result.Message);

		path.Key = RuntimeResourceKey::FromProjectPath(RuntimeResourceKind::Prefab, normalizedPath);
		if (!path.Key.CachePath.ends_with(".hprefab"))
			throw std::invalid_argument("Prefab resource path must use the .hprefab extension.");
		path.ResidentBytes = GetResidentBytes(path.Key, path.FullPath);
		return path;
	}

	RuntimePrefabPath ResolveRuntimePrefabWrittenPath(
		const std::filesystem::path& projectRoot,
		const std::filesystem::path& writtenPath)
	{
		if (!writtenPath.is_absolute())
			return ResolveRuntimePrefabPath(projectRoot, Utils::PathToUtf8(writtenPath));

		std::error_code error;
		const std::filesystem::path absoluteRoot = std::filesystem::absolute(projectRoot, error);
		if (error)
			throw std::invalid_argument("Cannot make the project root absolute: " + error.message());
		const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(absoluteRoot, error);
		if (error || !std::filesystem::is_directory(canonicalRoot, error) || error)
			throw std::invalid_argument("Cannot resolve the project root for prefab save.");

		const std::filesystem::path canonicalWritten =
			std::filesystem::weakly_canonical(writtenPath, error);
		if (error)
			throw std::invalid_argument("Cannot resolve the prefab save path: " + error.message());
		if (!IsInsideRoot(canonicalRoot, canonicalWritten))
			throw std::invalid_argument("Prefab save path resolves outside the project root.");

		const std::filesystem::path relative = canonicalWritten.lexically_relative(canonicalRoot);
		return ResolveRuntimePrefabPath(canonicalRoot, Utils::PathToUtf8(relative));
	}

	RuntimeResourceHandle<RuntimePrefabJson> AcquireRuntimePrefabJson(
		RuntimeResourceRegistry& registry,
		const RuntimePrefabPath& path)
	{
		return registry.AcquireBlocking<RuntimePrefabJson>(
			path.Key,
			[fullPath = path.FullPath]
			{
				return std::make_shared<RuntimePrefabJson>(Utils::LoadJsonFile(fullPath));
			},
			path.ResidentBytes);
	}

	bool QueueRuntimePrefabJson(
		RuntimeResourceRegistry& registry,
		const RuntimePrefabPath& path,
		RuntimeResourceGroup& group)
	{
		if (!group)
			throw std::logic_error("Prefab runtime resource queue requires a valid resource group.");

		return registry.QueueAsyncPinned<RuntimePrefabJson, Json>(
			path.Key,
			group,
			[fullPath = path.FullPath](std::stop_token stopToken)
			{
				if (stopToken.stop_requested())
					throw std::runtime_error("Prefab resource load was cancelled: " + Utils::PathToUtf8(fullPath));
				Json document = Utils::LoadJsonFile(fullPath);
				if (stopToken.stop_requested())
					throw std::runtime_error("Prefab resource load was cancelled: " + Utils::PathToUtf8(fullPath));
				return document;
			},
			[](Json&& document)
			{
				return std::make_shared<RuntimePrefabJson>(std::move(document));
			},
			path.ResidentBytes);
	}
}
