#include "Hepch.h"

#include "Runtime/Graphics/Mesh/MeshRuntimeResource.h"

#include "Runtime/Graphics/Mesh/Mesh.h"
#include "Runtime/Graphics/Mesh/MeshLoader.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceCatalog.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <limits>
#include <stdexcept>
#include <string>

namespace HEngine::MeshRuntimeResource
{
	namespace
	{
		struct MeshRequest
		{
			RuntimeResourceKey Key;
			std::filesystem::path FullPath;
			std::filesystem::path ProjectRoot;
			size_t ResidentBytes = 0;
		};

		[[nodiscard]] size_t GetResidentBytes(
			const std::shared_ptr<const RuntimeResourceCatalogSnapshot>& snapshot,
			const RuntimeResourceKey& key)
		{
			const RuntimeResourceCatalogEntry* entry = snapshot ? snapshot->Find(key.Path) : nullptr;
			if (!entry)
				return 0;

			constexpr uint64_t maxSize = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
			if (entry->CpuBytes > maxSize || entry->GpuBytes > maxSize - entry->CpuBytes)
				return std::numeric_limits<size_t>::max();
			return static_cast<size_t>(entry->CpuBytes + entry->GpuBytes);
		}

		[[nodiscard]] MeshRequest ResolveRequest(
			const std::filesystem::path& projectRoot,
			const std::shared_ptr<const RuntimeResourceCatalogSnapshot>& catalog,
			std::string_view projectPath)
		{
			if (projectRoot.empty())
				throw std::invalid_argument("Mesh runtime resource project root cannot be empty.");

			MeshRequest request;
			std::error_code error;
			const std::filesystem::path absoluteRoot = std::filesystem::absolute(projectRoot, error);
			if (error)
				throw std::invalid_argument("Cannot make the mesh project root absolute: " + error.message());
			request.ProjectRoot = std::filesystem::weakly_canonical(absoluteRoot, error);
			if (error || !std::filesystem::is_directory(request.ProjectRoot, error) || error)
				throw std::invalid_argument("Cannot resolve the mesh project root.");

			std::string normalizedPath;
			const Utils::PathValidationResult result = Utils::TryResolveProjectRelativePath(
				request.ProjectRoot,
				projectPath,
				request.FullPath,
				normalizedPath);
			if (!result)
				throw std::invalid_argument(result.Message);

			request.Key = RuntimeResourceKey::FromProjectPath(RuntimeResourceKind::Mesh, normalizedPath);
			request.ResidentBytes = GetResidentBytes(catalog, request.Key);
			return request;
		}

		[[nodiscard]] bool IsCookedMesh(const RuntimeResourceKey& key)
		{
			return key.CachePath.ends_with(".hmesh");
		}

		[[nodiscard]] bool QueueResolved(
			RuntimeResourceRegistry& registry,
			const MeshRequest& request,
			RuntimeResourceGroup& group)
		{
			if (!group)
				throw std::logic_error("Mesh runtime resource queue requires a valid resource group.");
			if (!IsCookedMesh(request.Key))
				return false;

			return registry.QueueAsyncPinned<Mesh, DecodedModel>(
				request.Key,
				group,
				[fullPath = request.FullPath, projectRoot = request.ProjectRoot](std::stop_token stopToken)
				{
					HE_PROFILE_SECTION("MeshLoader::LoadModel");
					MeshDecodeResult result =
						MeshLoader::DecodeModelHMESH(fullPath, projectRoot, stopToken);
					if (!result)
						throw std::runtime_error(result.Message);
					return std::move(result.Model);
				},
				[](DecodedModel&& decodedModel)
				{
					Ref<Mesh> mesh = CreateRef<Mesh>();
					MeshLoader::FinalizeModelHMESH(mesh.get(), std::move(decodedModel));
					return mesh;
				},
				request.ResidentBytes);
		}

		[[nodiscard]] Ref<Mesh> LoadBlocking(const MeshRequest& request)
		{
			Ref<Mesh> mesh = CreateRef<Mesh>();
			if (!IsCookedMesh(request.Key))
			{
				// Loose source formats remain a synchronous compatibility fallback.
				MeshLoader::LoadModel(mesh.get(), request.Key.Path);
				return mesh;
			}

			HE_PROFILE_SECTION("MeshLoader::LoadModel");
			MeshDecodeResult decoded = MeshLoader::DecodeModelHMESH(
				request.FullPath,
				request.ProjectRoot);
			if (!decoded)
				throw std::runtime_error(decoded.Message);
			MeshLoader::FinalizeModelHMESH(mesh.get(), std::move(decoded.Model));
			return mesh;
		}
	}

	RuntimeResourceKey MakeKey(
		const std::filesystem::path& projectRoot,
		std::string_view projectPath)
	{
		return ResolveRequest(projectRoot, nullptr, projectPath).Key;
	}

	bool QueueAsync(
		RuntimeResourceRegistry& registry,
		const std::filesystem::path& projectRoot,
		const std::shared_ptr<const RuntimeResourceCatalogSnapshot>& catalog,
		std::string_view projectPath,
		RuntimeResourceGroup& group)
	{
		if (!catalog)
			throw std::invalid_argument("Mesh runtime resource queue requires a catalog snapshot.");
		return QueueResolved(registry, ResolveRequest(projectRoot, catalog, projectPath), group);
	}

	Ref<Mesh> AcquirePrototype(
		RuntimeResourceRegistry& registry,
		const std::filesystem::path& projectRoot,
		const std::shared_ptr<const RuntimeResourceCatalogSnapshot>& catalog,
		std::string_view projectPath,
		RuntimeResourceGroup& group)
	{
		if (!group)
			throw std::logic_error("Mesh runtime resource acquisition requires a valid resource group.");
		if (!catalog)
			throw std::invalid_argument("Mesh runtime resource acquisition requires a catalog snapshot.");

		const MeshRequest request = ResolveRequest(projectRoot, catalog, projectPath);
		if (IsCookedMesh(request.Key))
			(void)QueueResolved(registry, request, group);

		RuntimeResourceHandle<Mesh> handle = registry.AcquireBlocking<Mesh>(
			request.Key,
			[request]
			{
				return LoadBlocking(request);
			},
			request.ResidentBytes);
		if (!group.Pin(handle))
			throw std::logic_error("Failed to pin mesh runtime resource: " + request.Key.Path);

		return handle.GetRef();
	}

	RuntimeResourceKey MakeKey(std::string_view projectPath)
	{
		const std::filesystem::path fullPath = ConfigManager::GetFullPath(std::string(projectPath));
		return MakeKey(ConfigManager::GetContentRootForPath(fullPath), projectPath);
	}

	bool QueueAsync(std::string_view projectPath, RuntimeResourceGroup& group)
	{
		return QueueAsync(
			RuntimeResourceRegistry::GetInstance(),
			ConfigManager::GetContentRootForPath(
				ConfigManager::GetFullPath(std::string(projectPath))),
			RuntimeResourceCatalog::GetInstance().GetSnapshot(),
			projectPath,
			group);
	}

	Ref<Mesh> AcquirePrototype(std::string_view projectPath, RuntimeResourceGroup& group)
	{
		return AcquirePrototype(
			RuntimeResourceRegistry::GetInstance(),
			ConfigManager::GetContentRootForPath(
				ConfigManager::GetFullPath(std::string(projectPath))),
			RuntimeResourceCatalog::GetInstance().GetSnapshot(),
			projectPath,
			group);
	}
}
