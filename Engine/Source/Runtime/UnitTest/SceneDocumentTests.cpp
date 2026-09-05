#include "Hepch.h"

#include "Runtime/UnitTest/SceneDocumentTests.h"

#include "Runtime/EcsFramework/Serializer/SceneDocument.h"
#include "Runtime/EcsFramework/Serializer/SceneSerializer.h"
#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceCatalog.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace HEngine::Tests
{
	namespace
	{
		void Require(bool condition, std::string_view message)
		{
			if (!condition)
				throw std::runtime_error(std::string(message));
		}

		void AppendNestedExceptionMessages(const std::exception& exception, std::string& messages)
		{
			if (!messages.empty())
				messages += " | ";
			messages += exception.what();
			try
			{
				std::rethrow_if_nested(exception);
			}
			catch (const std::exception& nested)
			{
				AppendNestedExceptionMessages(nested, messages);
			}
			catch (...)
			{
				messages += " | <non-standard exception>";
			}
		}

		template<typename Exception, typename Callback>
		void RequireThrows(Callback&& callback, std::string_view message)
		{
			bool bThrew = false;
			try
			{
				callback();
			}
			catch (const Exception&)
			{
				bThrew = true;
			}
			Require(bThrew, message);
		}

		class ScopedTempDirectory final
		{
		public:
			ScopedTempDirectory()
			{
				static std::atomic_uint64_t sCounter = 0;
				const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
				mPath = std::filesystem::temp_directory_path() /
					("HEngine_SceneDocument_" + std::to_string(timestamp) + "_" +
						std::to_string(sCounter.fetch_add(1, std::memory_order_relaxed)));
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

		Json MakeSceneJson(
			std::string_view firstMeshPath = "Assets\\Models//./Actor.hmesh",
			std::string_view secondMeshPath = "assets/models/actor.hmesh")
		{
			Json scene = Json::object();
			scene["10000000-0000-0000-0000-000000000001"] = {
				{ "MeshComponent", {
					{ "Path", std::string(firstMeshPath) },
					{ "meshResPath", "" }
				} },
				{ "TagComponent", { { "Tag", "First" } } }
			};
			scene["20000000-0000-0000-0000-000000000002"] = {
				{ "MeshComponent", {
					{ "Path", std::string(secondMeshPath) },
					{ "meshResPath", "" }
				} },
				{ "TagComponent", { { "Tag", "Second" } } }
			};
			return scene;
		}

		void VerifyStrictPathAndRegistryDedup(
			const std::filesystem::path& projectRoot,
			const std::filesystem::path& scenePath)
		{
			const SceneDocumentPath firstPath = ResolveSceneDocumentPath(
				projectRoot,
				"Assets\\Scenes//./Fixture.he");
			Require(firstPath.Key.Path == "Assets/Scenes/Fixture.he",
				"Scene path was not normalized to canonical project form.");
			Require(firstPath.Key.CachePath == "assets/scenes/fixture.he",
				"Scene cache key was not case folded.");
			Require(firstPath.ResidentBytes == std::filesystem::file_size(scenePath),
				"Resolved scene resident bytes did not match the source file.");

			const SceneDocumentPath aliasPath = ResolveSceneDocumentPath(
				projectRoot,
				"assets/scenes/fixture.he");
			Require(aliasPath.Key == firstPath.Key,
				"Equivalent scene aliases did not produce one registry key.");

			RequireThrows<std::invalid_argument>([&]
			{
				(void)ResolveSceneDocumentPath(projectRoot, "Assets/Scenes/../Fixture.he");
			}, "Scene parent traversal was accepted.");
			RequireThrows<std::invalid_argument>([&]
			{
				(void)ResolveSceneDocumentPath(projectRoot, scenePath.generic_string());
			}, "An absolute scene path was accepted.");
			RequireThrows<std::invalid_argument>([&]
			{
				(void)ResolveSceneDocumentPath(projectRoot, "Assets/Scenes/Fixture.json");
			}, "A non-.he document was accepted as a scene.");

			RuntimeResourceRegistry registry;
			registry.Init(2);
			{
				RuntimeResourceHandle<SceneDocument> first = AcquireSceneDocument(registry, firstPath);
				RuntimeResourceHandle<SceneDocument> alias = AcquireSceneDocument(registry, aliasPath);
				Require(first.GetGeneration() == alias.GetGeneration(),
					"Equivalent scene aliases created separate registry generations.");
				Require(registry.GetStats().PhysicalLoads == 1,
					"Equivalent scene aliases performed multiple physical loads.");
				Require(first->GetEntities().size() == 2,
					"The immutable scene document did not retain all entities.");
				Require(first->GetMeshPaths().size() == 1,
					"Equivalent scene mesh paths were not deduplicated.");
				Require(first->GetMeshPaths().front() == "Assets/Models/Actor.hmesh",
					"The deduplicated mesh path was not normalized deterministically.");

				first.Reset();
				alias.Reset();
				Require(registry.CollectUnused() == 1,
					"The unleased scene document was not reclaimed.");
			}
			registry.Shutdown();
		}

		void VerifyParseLimitsAndCancellation(
			const std::filesystem::path& projectRoot,
			const std::filesystem::path& scenePath)
		{
			const SceneDocumentPath path = ResolveSceneDocumentPath(
				projectRoot,
				"Assets/Scenes/Fixture.he");
			const SceneDocument document = ParseSceneDocument(path);
			Require(document.GetSourceBytes() == std::filesystem::file_size(scenePath),
				"Parsed scene source bytes did not match the bounded input.");
			Require(document.GetEntities().size() == 2,
				"A valid bounded scene did not retain all entities.");

			SceneDocumentLimits limits;
			limits.MaxFileBytes = document.GetSourceBytes() - 1;
			RequireThrows<std::runtime_error>([&]
			{
				(void)ParseSceneDocument(path, limits);
			}, "A scene larger than MaxFileBytes was accepted.");

			limits = {};
			limits.MaxEntities = 1;
			RequireThrows<std::runtime_error>([&]
			{
				(void)ParseSceneDocument(path, limits);
			}, "A scene larger than MaxEntities was accepted.");

			limits = {};
			limits.MaxComponentsPerEntity = 1;
			RequireThrows<std::runtime_error>([&]
			{
				(void)ParseSceneDocument(path, limits);
			}, "An entity larger than MaxComponentsPerEntity was accepted.");

			limits = {};
			limits.MaxJsonDepth = 2;
			RequireThrows<std::runtime_error>([&]
			{
				(void)ParseSceneDocument(path, limits);
			}, "A scene deeper than MaxJsonDepth was accepted.");

			limits = {};
			limits.MaxJsonNodes = 2;
			RequireThrows<std::runtime_error>([&]
			{
				(void)ParseSceneDocument(path, limits);
			}, "A scene larger than MaxJsonNodes was accepted.");

			limits = {};
			limits.MaxMeshPathBytes = 8;
			RequireThrows<std::runtime_error>([&]
			{
				(void)ParseSceneDocument(path, limits);
			}, "A mesh path larger than MaxMeshPathBytes was accepted.");

			const std::filesystem::path uniqueMeshPath =
				projectRoot / "Assets/Scenes/UniqueMeshes.he";
			Utils::SaveJsonToFile(
				MakeSceneJson("Assets/Models/First.hmesh", "Assets/Models/Second.hmesh"),
				uniqueMeshPath);
			const SceneDocumentPath uniquePath = ResolveSceneDocumentPath(
				projectRoot,
				"Assets/Scenes/UniqueMeshes.he");
			limits = {};
			limits.MaxUniqueMeshPaths = 1;
			RequireThrows<std::runtime_error>([&]
			{
				(void)ParseSceneDocument(uniquePath, limits);
			}, "A scene larger than MaxUniqueMeshPaths was accepted.");

			const std::filesystem::path sourceMeshPath =
				projectRoot / "Assets/Scenes/SourceMeshes.he";
			Utils::SaveJsonToFile(
				MakeSceneJson("Assets/Models/Actor.fbx", "Assets/Models/Actor.obj"),
				sourceMeshPath);
			const SceneDocument sourceMeshDocument = ParseSceneDocument(
				ResolveSceneDocumentPath(projectRoot, "Assets/Scenes/SourceMeshes.he"));
			Require(sourceMeshDocument.GetEntities().size() == 2 &&
				sourceMeshDocument.GetMeshPaths().empty(),
				"Loose source meshes were incorrectly added to the async .hmesh dependency list.");

			const std::filesystem::path unsafeSourceMeshPath =
				projectRoot / "Assets/Scenes/UnsafeSourceMesh.he";
			Utils::SaveJsonToFile(
				MakeSceneJson("../Outside.fbx", "None"),
				unsafeSourceMeshPath);
			RequireThrows<std::runtime_error>([&]
			{
				(void)ParseSceneDocument(ResolveSceneDocumentPath(
					projectRoot,
					"Assets/Scenes/UnsafeSourceMesh.he"));
			}, "A traversing loose source mesh path bypassed scene prescan validation.");

			const std::filesystem::path invalidUuidPath =
				projectRoot / "Assets/Scenes/InvalidUuid.he";
			Json invalidUuidScene = Json::object();
			invalidUuidScene["not-a-valid-uuid"] = {
				{ "TagComponent", { { "Tag", "Invalid" } } }
			};
			Utils::SaveJsonToFile(invalidUuidScene, invalidUuidPath);
			const SceneDocumentPath invalidUuidDocumentPath = ResolveSceneDocumentPath(
				projectRoot,
				"Assets/Scenes/InvalidUuid.he");
			RequireThrows<std::runtime_error>([&]
			{
				(void)ParseSceneDocument(invalidUuidDocumentPath);
			}, "An entity with an invalid retained UUID was accepted.");

			std::stop_source cancelledSource;
			cancelledSource.request_stop();
			RequireThrows<std::runtime_error>([&]
			{
				(void)ParseSceneDocument(path, {}, cancelledSource.get_token());
			}, "A pre-cancelled scene parse unexpectedly published a document.");
		}

		SceneDocumentEntity MakeMaterializationEntity(
			std::string uuid,
			std::string tag,
			bool bInvalidPythonPath = false)
		{
			Json value = {
				{ "TagComponent", { { "Tag", tag } } }
			};
			if (bInvalidPythonPath)
				value["PythonScriptComponent"] = { { "Path", 7 } };
			return {
				std::move(uuid),
				std::move(tag),
				std::move(value)
			};
		}

		std::shared_ptr<const SceneDocument> MakeMaterializationDocument(
			bool bInvalidSecondEntity = false)
		{
			std::vector<SceneDocumentEntity> entities;
			entities.emplace_back(MakeMaterializationEntity(
				"30000000-0000-0000-0000-000000000003",
				"First"));
			entities.emplace_back(MakeMaterializationEntity(
				"40000000-0000-0000-0000-000000000004",
				"Second",
				bInvalidSecondEntity));
			return std::make_shared<SceneDocument>(
				std::move(entities),
				std::vector<std::string>{},
				128);
		}

		size_t CountEntities(const Ref<Level>& level)
		{
			size_t entityCount = 0;
			level->mRegistry.each([&](auto)
			{
				++entityCount;
			});
			return entityCount;
		}

		SceneMaterializationBudget OneEntityBudget()
		{
			return {
				std::chrono::nanoseconds::zero(),
				1
			};
		}

		void StartMaterialization(
			IncrementalSceneMaterializer& materializer,
			RuntimeResourceRegistry& registry,
			std::shared_ptr<const SceneDocument> document,
			const std::filesystem::path& projectRoot)
		{
			materializer.Start(
				registry,
				std::move(document),
				projectRoot,
				RuntimeResourceCatalogSnapshot::Empty());
		}

		void VerifyIncrementalMaterializationSuccess(
			RuntimeResourceRegistry& registry,
			const std::filesystem::path& projectRoot)
		{
			IncrementalSceneMaterializer materializer;
			StartMaterialization(materializer, registry, MakeMaterializationDocument(), projectRoot);

			SceneMaterializationProgress progress = materializer.GetProgress();
			Require(progress.State == SceneMaterializationState::WaitingForResources,
				"A new materialization did not begin in the resource wait state.");
			Require(progress.TotalEntities == 2 && progress.MaterializedEntities == 0,
				"A new materialization reported the wrong entity totals.");

			progress = materializer.Pump(OneEntityBudget());
			Require(progress.State == SceneMaterializationState::Materializing,
				"The first bounded pump did not remain in the materializing state.");
			Require(progress.MaterializedEntities == 1,
				"A maxEntities=1 pump materialized more or fewer than one entity.");
			Require(!materializer.TakeCompletedLevel(),
				"A partial staging level was published.");

			const SceneMaterializationProgress beforeRestart = materializer.GetProgress();
			RequireThrows<std::logic_error>([&]
			{
				StartMaterialization(materializer, registry, MakeMaterializationDocument(), projectRoot);
			}, "An active materialization accepted a second Start request.");
			const SceneMaterializationProgress afterRestart = materializer.GetProgress();
			Require(afterRestart.State == beforeRestart.State &&
				afterRestart.TotalEntities == beforeRestart.TotalEntities &&
				afterRestart.MaterializedEntities == beforeRestart.MaterializedEntities &&
				afterRestart.Resources.Total == beforeRestart.Resources.Total &&
				afterRestart.Resources.Ready == beforeRestart.Resources.Ready,
				"A rejected second Start modified the active materialization transaction.");

			progress = materializer.Pump(OneEntityBudget());
			Require(progress.State == SceneMaterializationState::Ready &&
				progress.MaterializedEntities == 2 && progress.Ratio == 1.0f,
				"The second bounded pump did not complete the scene exactly.");

			Ref<Level> completedLevel = materializer.TakeCompletedLevel();
			Require(completedLevel && CountEntities(completedLevel) == 2,
				"The completed level did not contain exactly the staged entities.");
			Require(!materializer.TakeCompletedLevel(),
				"TakeCompletedLevel published the completed scene more than once.");
			completedLevel.reset();
		}

		void VerifyFailedMaterializationDoesNotPublish(
			RuntimeResourceRegistry& registry,
			const std::filesystem::path& projectRoot)
		{
			IncrementalSceneMaterializer materializer;
			StartMaterialization(materializer, registry, MakeMaterializationDocument(true), projectRoot);
			SceneMaterializationProgress progress = materializer.Pump(OneEntityBudget());
			Require(progress.State == SceneMaterializationState::Materializing &&
				progress.MaterializedEntities == 1,
				"The failure fixture did not stage its first entity incrementally.");

			progress = materializer.Pump(OneEntityBudget());
			Require(progress.State == SceneMaterializationState::Failed &&
				progress.MaterializedEntities == 1,
				"A component decode failure did not preserve deterministic progress.");
			Require(!materializer.TakeCompletedLevel(),
				"A failed staging level was published.");
			RequireThrows<std::runtime_error>([&]
			{
				materializer.RethrowFailure();
			}, "A failed materialization did not retain its failure context.");
		}

		void VerifyCancelledMaterializationDoesNotPublish(
			RuntimeResourceRegistry& registry,
			const std::filesystem::path& projectRoot)
		{
			IncrementalSceneMaterializer materializer;
			StartMaterialization(materializer, registry, MakeMaterializationDocument(), projectRoot);
			const SceneMaterializationProgress progress = materializer.Pump(OneEntityBudget());
			Require(progress.State == SceneMaterializationState::Materializing &&
				progress.MaterializedEntities == 1,
				"The cancellation fixture did not stage one entity first.");

			materializer.RequestCancel();
			const SceneMaterializationProgress cancelledProgress = materializer.GetProgress();
			Require(cancelledProgress.State == SceneMaterializationState::Cancelled &&
				cancelledProgress.MaterializedEntities == 1,
				"Cancellation did not retain deterministic partial progress.");
			Require(!materializer.TakeCompletedLevel(),
				"A cancelled staging level was published.");
			RequireThrows<std::runtime_error>([&]
			{
				materializer.RethrowFailure();
			}, "A cancelled materialization did not retain cancellation context.");
		}

		void VerifyNonCookedMeshDependencyIsNotQueued(
			RuntimeResourceRegistry& registry,
			const std::filesystem::path& projectRoot)
		{
			std::vector<SceneDocumentEntity> entities;
			entities.emplace_back(MakeMaterializationEntity(
				"50000000-0000-0000-0000-000000000005",
				"ForgedMesh"));
			std::shared_ptr<const SceneDocument> forgedDocument = std::make_shared<SceneDocument>(
				std::move(entities),
				std::vector<std::string>{ "Assets/Models/Forged.fbx" },
				64);

			IncrementalSceneMaterializer materializer;
			StartMaterialization(materializer, registry, std::move(forgedDocument), projectRoot);
			Require(materializer.GetProgress().Resources.Total == 0,
				"A loose source mesh was incorrectly queued as an async dependency.");
			const SceneMaterializationProgress progress = materializer.Pump(OneEntityBudget());
			Require(progress.State == SceneMaterializationState::Ready,
				"A safe loose source mesh dependency blocked staged materialization.");
			Ref<Level> completedLevel = materializer.TakeCompletedLevel();
			Require(static_cast<bool>(completedLevel),
				"A scene with a non-queued loose source mesh dependency was not published.");
		}

		void VerifyInjectedMeshFailureUsesProvidedRegistry(
			RuntimeResourceRegistry& registry,
			const std::filesystem::path& projectRoot)
		{
			std::shared_ptr<const SceneDocument> document = std::make_shared<SceneDocument>(
				std::vector<SceneDocumentEntity>{},
				std::vector<std::string>{ "Assets/Models/Missing.hmesh" },
				32);
			IncrementalSceneMaterializer materializer;
			StartMaterialization(materializer, registry, std::move(document), projectRoot);
			Require(materializer.GetProgress().State == SceneMaterializationState::WaitingForResources,
				"An injected mesh dependency did not enter the resource wait state.");

			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (materializer.GetProgress().State == SceneMaterializationState::WaitingForResources)
			{
				(void)registry.PumpMainThreadWork(std::chrono::milliseconds(10), 8);
				(void)materializer.Pump(OneEntityBudget());
				if (std::chrono::steady_clock::now() >= deadline)
					throw std::runtime_error("Timed out waiting for an injected mesh failure.");
			}
			Require(materializer.GetProgress().State == SceneMaterializationState::Failed,
				"An injected missing mesh did not fail through the provided registry.");
			std::string failureMessages;
			try
			{
				materializer.RethrowFailure();
			}
			catch (const std::exception& exception)
			{
				AppendNestedExceptionMessages(exception, failureMessages);
			}
			Require(
				failureMessages.find("A staged scene mesh failed to load: Assets/Models/Missing.hmesh") !=
					std::string::npos,
				"An injected mesh failure did not retain its mesh-path context.");
			Require(
				failureMessages.find("Mesh metadata is missing or is not a regular file") !=
					std::string::npos,
				"An injected mesh failure did not retain its registry loader exception.");
		}

		void VerifyIncrementalMaterialization(const std::filesystem::path& projectRoot)
		{
			RuntimeResourceRegistry registry;
			registry.Init(1);
			VerifyIncrementalMaterializationSuccess(registry, projectRoot);
			VerifyFailedMaterializationDoesNotPublish(registry, projectRoot);
			VerifyCancelledMaterializationDoesNotPublish(registry, projectRoot);
			VerifyNonCookedMeshDependencyIsNotQueued(registry, projectRoot);
			VerifyInjectedMeshFailureUsesProvidedRegistry(registry, projectRoot);
			registry.Shutdown();
		}
	}

	void SceneDocumentContract()
	{
		ScopedTempDirectory tempDirectory;
		const std::filesystem::path projectRoot = tempDirectory.Get() / "Project";
		const std::filesystem::path sceneDirectory = projectRoot / "Assets/Scenes";
		std::filesystem::create_directories(sceneDirectory);

		const std::filesystem::path scenePath = sceneDirectory / "Fixture.he";
		Utils::SaveJsonToFile(MakeSceneJson(), scenePath);

		VerifyStrictPathAndRegistryDedup(projectRoot, scenePath);
		VerifyParseLimitsAndCancellation(projectRoot, scenePath);
		VerifyIncrementalMaterialization(projectRoot);
	}
}
