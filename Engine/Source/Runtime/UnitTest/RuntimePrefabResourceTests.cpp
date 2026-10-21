#include "Hepch.h"

#include "Runtime/UnitTest/RuntimePrefabResourceTests.h"

#include "Runtime/EcsFramework/Component/Basic/IDComponent.h"
#include "Runtime/EcsFramework/Component/Basic/TagComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Serializer/PrefabSerializer.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimePrefabResource.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace HEngine::Tests
{
	namespace
	{
		void Require(bool condition, std::string_view message)
		{
			if (!condition)
				throw std::runtime_error(std::string(message));
		}

		class ScopedTempDirectory final
		{
		public:
			ScopedTempDirectory()
			{
				const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
				mPath = std::filesystem::temp_directory_path() /
					("HEngine_RuntimePrefabResource_" + std::to_string(suffix));
				std::filesystem::create_directories(mPath);
			}

			~ScopedTempDirectory()
			{
				std::error_code error;
				std::filesystem::remove_all(mPath, error);
			}

			[[nodiscard]] const std::filesystem::path& Get() const noexcept
			{
				return mPath;
			}

		private:
			std::filesystem::path mPath;
		};

		template<typename Callback>
		void RequireInvalidArgument(Callback&& callback, std::string_view message)
		{
			bool bThrew = false;
			try
			{
				callback();
			}
			catch (const std::invalid_argument&)
			{
				bThrew = true;
			}
			Require(bThrew, message);
		}

		[[nodiscard]] size_t CountEntities(Level& level)
		{
			size_t count = 0;
			for (const auto entity : level.GetAllEntitiesWith<IDComponent>())
			{
				(void)entity;
				++count;
			}
			return count;
		}

		class ScopedConfigRoot final
		{
		public:
			ScopedConfigRoot(
				const std::filesystem::path& projectRoot,
				const std::filesystem::path& engineContentRoot)
			{
				ConfigManager::GetInstance().Init(projectRoot, engineContentRoot);
			}

			~ScopedConfigRoot()
			{
				ConfigManager::GetInstance().Clear();
			}
		};
	}

	void RuntimePrefabResourceContract()
	{
		ScopedTempDirectory tempDirectory;
		const std::filesystem::path projectRoot = tempDirectory.Get() / "Project";
		const std::filesystem::path prefabDirectory = projectRoot / "Assets" / "Prefabs";
		std::filesystem::create_directories(prefabDirectory);

		const std::filesystem::path prefabPath = prefabDirectory / "Actor.hprefab";
		Utils::SaveJsonToFile(Json{ { "Version", 1 }, { "Name", "First" } }, prefabPath);

		const RuntimePrefabPath firstPath = ResolveRuntimePrefabPath(
			projectRoot,
			"Assets\\Prefabs//./Actor.hprefab");
		Require(firstPath.Key.Path == "Assets/Prefabs/Actor.hprefab",
			"Prefab path was not normalized to the canonical project form.");
		Require(firstPath.Key.CachePath == "assets/prefabs/actor.hprefab",
			"Prefab cache key was not case folded.");

		const RuntimePrefabPath aliasPath = ResolveRuntimePrefabPath(
			projectRoot,
			"assets/prefabs/actor.hprefab");
		Require(aliasPath.Key == firstPath.Key,
			"Equivalent prefab path aliases did not produce one registry key.");

		const RuntimePrefabPath writtenPath = ResolveRuntimePrefabWrittenPath(projectRoot, prefabPath);
		Require(writtenPath.Key == firstPath.Key,
			"Absolute project-local prefab save path did not resolve to the load key.");
		const RuntimePrefabPath newWrittenPath = ResolveRuntimePrefabWrittenPath(
			projectRoot,
			prefabDirectory / "NewActor.hprefab");
		Require(newWrittenPath.Key.Path == "Assets/Prefabs/NewActor.hprefab",
			"A project-local non-existing prefab save target was rejected.");

		RequireInvalidArgument([&]
		{
			(void)ResolveRuntimePrefabPath(projectRoot, "Assets/Prefabs/../Actor.hprefab");
		}, "Prefab parent traversal was accepted.");
		RequireInvalidArgument([&]
		{
			(void)ResolveRuntimePrefabPath(projectRoot, prefabPath.generic_string());
		}, "Absolute prefab load path was accepted.");
		RequireInvalidArgument([&]
		{
			(void)ResolveRuntimePrefabPath(projectRoot, "Assets/Prefabs/Actor.json");
		}, "A non-.hprefab resource was accepted as a prefab.");
		RequireInvalidArgument([&]
		{
			(void)ResolveRuntimePrefabWrittenPath(
				projectRoot,
				tempDirectory.Get() / "Outside.hprefab");
		}, "A prefab save target outside the project root was accepted.");

		RuntimeResourceRegistry registry;
		registry.Init(2);
		{
			RuntimeResourceGroup group = registry.CreateGroup();
			RuntimeResourceHandle<RuntimePrefabJson> first =
				AcquireRuntimePrefabJson(registry, firstPath);
			const uint64_t firstGeneration = first.GetGeneration();
			Require(first->GetDocument().at("Name") == "First",
				"Initial prefab document was not loaded.");

			RuntimeResourceHandle<RuntimePrefabJson> alias =
				AcquireRuntimePrefabJson(registry, aliasPath);
			Require(alias.GetGeneration() == firstGeneration,
				"Prefab alias created a second registry generation.");
			Require(registry.GetStats().PhysicalLoads == 1,
				"Prefab aliases performed more than one physical load.");
			Require(group.Pin(first), "Failed to pin the initial prefab generation.");

			first.Reset();
			alias.Reset();
			Require(registry.CollectUnused() == 0,
				"A group-pinned prefab generation was evicted.");

			Utils::SaveJsonToFile(Json{ { "Version", 2 }, { "Name", "Second" } }, prefabPath);
			RuntimeResourceHandle<RuntimePrefabJson> cached =
				AcquireRuntimePrefabJson(registry, firstPath);
			Require(cached->GetDocument().at("Name") == "First",
				"Prefab changed without an explicit generation invalidation.");
			cached.Reset();

			Require(registry.Invalidate(writtenPath.Key),
				"Loaded prefab generation could not be invalidated after save.");
			RuntimeResourceHandle<RuntimePrefabJson> updated =
				AcquireRuntimePrefabJson(registry, firstPath);
			Require(updated.GetGeneration() != firstGeneration,
				"Prefab invalidation did not create a replacement generation.");
			Require(updated->GetDocument().at("Name") == "Second",
				"Replacement prefab generation did not read the updated file.");
			Require(group.Pin(updated), "Failed to pin the replacement prefab generation.");

			const std::filesystem::path asyncFullPath = prefabDirectory / "Async.hprefab";
			Utils::SaveJsonToFile(Json{ { "Version", 1 }, { "Name", "Async" } }, asyncFullPath);
			const RuntimePrefabPath asyncPath = ResolveRuntimePrefabPath(
				projectRoot,
				"Assets/Prefabs/Async.hprefab");
			Require(QueueRuntimePrefabJson(registry, asyncPath, group),
				"Prefab async request did not queue its physical load.");
			RuntimeResourceHandle<RuntimePrefabJson> asyncHandle =
				AcquireRuntimePrefabJson(registry, asyncPath);
			Require(asyncHandle->GetDocument().at("Name") == "Async",
				"Prefab async load did not finalize its immutable document.");
			Require(registry.GetStats().PhysicalLoads == 3,
				"Prefab load/invalidate/async physical load count was not deterministic.");

			updated.Reset();
			asyncHandle.Reset();
			group.Release();
			Require(registry.CollectUnused() == 3,
				"Released prefab generations were not reclaimed on the init thread.");
			Require(registry.GetStats().Resident == 0,
				"Prefab resources remained resident after all leases and pins were released.");
		}
		registry.Shutdown();

		const std::filesystem::path configDirectory = projectRoot / "Config";
		std::filesystem::create_directories(configDirectory);
		Utils::SaveJsonToFile(
			Json{ { "Renderer", { { "API", "DX12" }, { "VSync", false } } } },
			configDirectory / "BaseEngine.json");
		Utils::SaveJsonToFile(Json{ { "Game", Json::object() } }, configDirectory / "Game.json");
		const std::filesystem::path engineContentRoot =
			tempDirectory.Get() / "Engine" / "Content";
		std::filesystem::create_directories(engineContentRoot);
		const std::filesystem::path brokenPrefabPath = prefabDirectory / "Broken.hprefab";
		Utils::SaveJsonToFile(
			Json{
				{ "TagComponent", { { "Tag", "Broken" } } },
				{ "PythonScriptComponent", { { "Path", 7 } } }
			},
			brokenPrefabPath);

		ScopedConfigRoot configRoot(projectRoot, engineContentRoot);
		Level level;
		Require(CountEntities(level) == 0,
			"The prefab rollback fixture did not begin with an empty level.");
		bool bInstantiationFailed = false;
		try
		{
			(void)PrefabSerializer::InstantiatePrefab(
				&level,
				"Assets/Prefabs/Broken.hprefab");
		}
		catch (const std::exception&)
		{
			bInstantiationFailed = true;
		}
		Require(bInstantiationFailed,
			"An invalid prefab component unexpectedly instantiated successfully.");
		Require(CountEntities(level) == 0,
			"A failed prefab deserialization leaked its partially created entity.");

		const auto cachedPath = prefabDirectory / "Cached.hprefab";
		const std::string cachedName = "Assets/Prefabs/Cached.hprefab";
		Utils::SaveJsonToFile(Json{{"TagComponent", {{"Tag", "Before"}}}}, cachedPath);
		auto& runtimeRegistry = RuntimeResourceRegistry::GetInstance();
		runtimeRegistry.Init(0);
		{
			Level cachedLevel;
			Entity first = PrefabSerializer::InstantiatePrefab(&cachedLevel, cachedName);
			Entity second = PrefabSerializer::InstantiatePrefab(&cachedLevel, cachedName);
			Require(first && second && first.GetComponent<IDComponent>().ID != second.GetComponent<IDComponent>().ID,
				"Prepared prefab instances reused an entity identity.");
			Require(first.GetComponent<TagComponent>().Tag == "Before" &&
				second.GetComponent<TagComponent>().Tag == "Before",
				"Prepared prefab instances lost the original component value.");
			Utils::SaveJsonToFile(Json{{"TagComponent", {{"Tag", "After"}}}}, cachedPath);
			Require(runtimeRegistry.Invalidate(
				ResolveRuntimePrefabPath(projectRoot, cachedName).Key),
				"Prepared prefab fixture could not invalidate its resource generation.");
			Entity third = PrefabSerializer::InstantiatePrefab(&cachedLevel, cachedName);
			Require(third && third.GetComponent<TagComponent>().Tag == "After" &&
				first.GetComponent<TagComponent>().Tag == "Before",
				"Prepared prefab cache survived a resource generation change.");
		}
		runtimeRegistry.Shutdown();
	}
}
