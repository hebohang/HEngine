#include "Hepch.h"

#include "Runtime/UnitTest/RuntimeResourceWarmupTests.h"

#include "Runtime/Resource/RuntimeProjectResourceWarmup.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeResourceWarmupManager.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

		class ScopedTempDirectory final
		{
		public:
			ScopedTempDirectory()
			{
				const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
				mPath = std::filesystem::temp_directory_path() /
					("HEngine_RuntimeResourceWarmup_" + std::to_string(suffix));
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

		struct HeadlessWarmupResource
		{
			Json Document;
		};

		[[nodiscard]] RuntimeResourceKind ToHeadlessResourceKind(
			RuntimeCatalogResourceType type)
		{
			switch (type)
			{
			case RuntimeCatalogResourceType::Scene: return RuntimeResourceKind::Scene;
			case RuntimeCatalogResourceType::Mesh: return RuntimeResourceKind::Mesh;
			case RuntimeCatalogResourceType::Prefab: return RuntimeResourceKind::Prefab;
			case RuntimeCatalogResourceType::UiDocument: return RuntimeResourceKind::Ui;
			default:
				throw std::logic_error("Headless warm-up dispatch received an unsupported target type.");
			}
		}

		[[nodiscard]] RuntimeResourceKey QueueHeadlessWarmupTarget(
			RuntimeResourceRegistry& registry,
			const std::filesystem::path& projectRoot,
			const RuntimeResourceWarmupTarget& target,
			RuntimeResourceGroup& group)
		{
			std::filesystem::path fullPath;
			std::string normalizedPath;
			const Utils::PathValidationResult pathResult = Utils::TryResolveProjectRelativePath(
				projectRoot,
				target.Path,
				fullPath,
				normalizedPath);
			if (!pathResult)
				throw std::invalid_argument(pathResult.Message);

			const RuntimeResourceKey key = RuntimeResourceKey::FromProjectPath(
				ToHeadlessResourceKind(target.Type),
				normalizedPath);
			(void)registry.QueueAsyncPinned<HeadlessWarmupResource, Json>(
				key,
				group,
				[fullPath](std::stop_token stopToken)
				{
					if (stopToken.stop_requested())
						throw std::runtime_error("Headless warm-up load was cancelled.");
					Json document = Utils::LoadJsonFile(fullPath);
					if (stopToken.stop_requested())
						throw std::runtime_error("Headless warm-up load was cancelled.");
					return document;
				},
				[](Json&& document)
				{
					return std::make_shared<HeadlessWarmupResource>(
						HeadlessWarmupResource{ std::move(document) });
				});
			return key;
		}

		[[nodiscard]] RuntimeResourceWarmupDispatch MakeHeadlessWarmupDispatch(
			RuntimeResourceRegistry& registry,
			std::filesystem::path projectRoot)
		{
			return [&registry, projectRoot = std::move(projectRoot)](
				const RuntimeResourceWarmupTarget& target,
				RuntimeResourceGroup& group)
			{
				return QueueHeadlessWarmupTarget(registry, projectRoot, target, group);
			};
		}

		[[nodiscard]] RuntimeResourceHandle<HeadlessWarmupResource> AcquireHeadlessWarmupTarget(
			RuntimeResourceRegistry& registry,
			const std::filesystem::path& projectRoot,
			const RuntimeResourceWarmupTarget& target)
		{
			std::filesystem::path fullPath;
			std::string normalizedPath;
			const Utils::PathValidationResult pathResult = Utils::TryResolveProjectRelativePath(
				projectRoot,
				target.Path,
				fullPath,
				normalizedPath);
			if (!pathResult)
				throw std::invalid_argument(pathResult.Message);

			const RuntimeResourceKey key = RuntimeResourceKey::FromProjectPath(
				ToHeadlessResourceKind(target.Type),
				normalizedPath);
			return registry.AcquireBlocking<HeadlessWarmupResource>(
				key,
				[fullPath]
				{
					return std::make_shared<HeadlessWarmupResource>(
						HeadlessWarmupResource{ Utils::LoadJsonFile(fullPath) });
				});
		}

		Json MakeGroup(
			std::string name,
			std::vector<std::string> roots,
			std::string trigger = {},
			int priority = 0,
			std::string residency = "Session")
		{
			Json rootArray = Json::array();
			for (std::string& root : roots)
				rootArray.push_back({ { "Path", std::move(root) } });

			Json group = {
				{ "Name", name },
				{ "Priority", priority },
				{ "Residency", std::move(residency) },
				{ "Roots", std::move(rootArray) }
			};
			if (!trigger.empty())
				group["Trigger"] = std::move(trigger);
			return group;
		}

		Json MakeGame(Json groups)
		{
			return Json{
				{ "ResourceWarmup", {
					{ "Version", 1 },
					{ "Groups", std::move(groups) }
				} }
			};
		}

		RuntimeResourceCatalogEntry MakeCatalogEntry(
			std::string path,
			RuntimeCatalogResourceType type,
			std::vector<RuntimeCatalogDependency> dependencies,
			std::vector<std::string> tags)
		{
			RuntimeResourceCatalogEntry entry;
			entry.Path = std::move(path);
			entry.Type = type;
			entry.Dependencies = std::move(dependencies);
			entry.Tags = std::move(tags);
			return entry;
		}

		std::shared_ptr<const RuntimeResourceCatalogSnapshot> MakeSnapshot(
			std::vector<RuntimeResourceCatalogEntry> entries)
		{
			const std::string hash = RuntimeResourceCatalogSnapshot::ComputeRegistryHash(entries);
			return RuntimeResourceCatalogSnapshot::Create(std::move(entries), hash);
		}

		void PumpUntilTerminal(
			RuntimeResourceWarmupManager& manager,
			RuntimeResourceRegistry& registry,
			std::string_view groupName)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			for (;;)
			{
				manager.Pump();
				(void)registry.PumpMainThreadWork(std::chrono::milliseconds(10), 8);
				manager.Pump();

				const auto progress = manager.GetGroupProgress(groupName);
				Require(progress.has_value(), "Warm-up group progress disappeared while pumping.");
				if (progress->State == RuntimeResourceWarmupGroupState::Ready ||
					progress->State == RuntimeResourceWarmupGroupState::Failed)
				{
					return;
				}
				if (std::chrono::steady_clock::now() >= deadline)
					throw std::runtime_error("Timed out waiting for a warm-up group.");
				std::this_thread::yield();
			}
		}

		void PumpProjectWarmupUntilTerminal(
			RuntimeProjectResourceWarmup& warmup,
			RuntimeResourceRegistry& registry,
			std::string_view groupName)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			for (;;)
			{
				warmup.Pump();
				(void)registry.PumpMainThreadWork(std::chrono::milliseconds(10), 8);
				warmup.Pump();

				const auto progress = warmup.GetGroupProgress(groupName);
				Require(progress.has_value(), "Project warm-up group progress disappeared while pumping.");
				if (progress->State == RuntimeResourceWarmupGroupState::Ready ||
					progress->State == RuntimeResourceWarmupGroupState::Failed)
				{
					return;
				}
				if (std::chrono::steady_clock::now() >= deadline)
					throw std::runtime_error("Timed out waiting for a project warm-up group.");
				std::this_thread::yield();
			}
		}

		void PlanContract(const std::filesystem::path& projectRoot)
		{
			const RuntimeResourceWarmupPlan absent = BuildRuntimeWarmupPlan(
				Json::object(),
				RuntimeResourceCatalogSnapshot::Empty(),
				projectRoot);
			Require(absent.Groups.empty(),
				"An omitted optional ResourceWarmup object produced schema groups.");

			const Json looseGame = MakeGame(Json::array({
				Json{
					{ "Name", "Defaults" },
					{ "Roots", Json::array({ Json{ { "Path", "Assets\\Prefabs//./A.hprefab" } } }) }
				}
			}));
			const RuntimeResourceWarmupPlan loose = BuildRuntimeWarmupPlan(
				looseGame,
				RuntimeResourceCatalogSnapshot::Empty(),
				projectRoot);
			Require(!loose.bPackaged && loose.Groups.size() == 1,
				"Loose warm-up planning did not produce its configured group.");
			const RuntimeResourceWarmupGroupPlan& defaults = loose.Groups.front();
			Require(defaults.Trigger == "Defaults" && defaults.Priority == 0 &&
				defaults.Expand == RuntimeResourceWarmupExpand::HardRuntime &&
				defaults.Residency == RuntimeResourceWarmupResidency::Session,
				"Warm-up group defaults diverged from the cooker contract.");
			Require(defaults.Targets.size() == 1 &&
				defaults.Targets.front().Path == "Assets/Prefabs/A.hprefab" &&
				defaults.Targets.front().Type == RuntimeCatalogResourceType::Prefab,
				"Loose warm-up root was not normalized and classified.");

			Json explicitValues = MakeGame(Json::array({ Json{
				{ "Name", "  Explicit  " },
				{ "Trigger", "  OnDemand  " },
				{ "Priority", -7 },
				{ "Expand", "None" },
				{ "Residency", "Startup" },
				{ "Roots", Json::array({ Json{ { "Path", "Assets/Prefabs/A.hprefab" } } }) }
			} }));
			const RuntimeResourceWarmupPlan explicitPlan = BuildRuntimeWarmupPlan(
				explicitValues,
				RuntimeResourceCatalogSnapshot::Empty(),
				projectRoot);
			const RuntimeResourceWarmupGroupPlan& explicitGroup = explicitPlan.Groups.front();
			Require(explicitGroup.Name == "Explicit" && explicitGroup.Trigger == "OnDemand" &&
				explicitGroup.Priority == -7 &&
				explicitGroup.Expand == RuntimeResourceWarmupExpand::None &&
				explicitGroup.Residency == RuntimeResourceWarmupResidency::Startup &&
				explicitGroup.Roots.front().Expand == RuntimeResourceWarmupExpand::None,
				"Explicit warm-up schema values or inherited root defaults diverged.");

			const auto RequireSchemaFailure = [&](Json game, std::string_view message)
			{
				RequireInvalidArgument([&]
				{
					(void)BuildRuntimeWarmupPlan(
						game,
						RuntimeResourceCatalogSnapshot::Empty(),
						projectRoot);
				}, message);
			};

			Json invalidSchema = MakeGame(Json::array());
			invalidSchema["ResourceWarmup"]["Version"] = 2;
			RequireSchemaFailure(std::move(invalidSchema),
				"ResourceWarmup accepted an unsupported schema version.");
			invalidSchema = MakeGame(Json::array());
			invalidSchema["ResourceWarmup"]["Version"] = "1";
			RequireSchemaFailure(std::move(invalidSchema),
				"ResourceWarmup accepted a non-integer schema version.");
			invalidSchema = MakeGame(Json::array());
			invalidSchema["ResourceWarmup"]["Unknown"] = true;
			RequireSchemaFailure(std::move(invalidSchema),
				"ResourceWarmup accepted an unknown top-level field.");

			Json invalidGroup = MakeGroup("Group", { "Assets/Prefabs/A.hprefab" });
			invalidGroup["Unknown"] = true;
			RequireSchemaFailure(MakeGame(Json::array({ std::move(invalidGroup) })),
				"ResourceWarmup accepted an unknown group field.");
			invalidGroup = MakeGroup("Group", { "Assets/Prefabs/A.hprefab" });
			invalidGroup["Roots"][0]["Unknown"] = true;
			RequireSchemaFailure(MakeGame(Json::array({ std::move(invalidGroup) })),
				"ResourceWarmup accepted an unknown root field.");
			invalidGroup = MakeGroup("Group", { "Assets/Prefabs/A.hprefab" });
			invalidGroup["Priority"] = 1.5;
			RequireSchemaFailure(MakeGame(Json::array({ std::move(invalidGroup) })),
				"ResourceWarmup accepted a non-integer priority.");
			invalidGroup = MakeGroup("Group", { "Assets/Prefabs/A.hprefab" });
			invalidGroup["Priority"] = std::numeric_limits<std::int64_t>::max();
			RequireSchemaFailure(MakeGame(Json::array({ std::move(invalidGroup) })),
				"ResourceWarmup accepted a priority outside int32.");
			invalidGroup = MakeGroup("Group", { "Assets/Prefabs/A.hprefab" });
			invalidGroup["Priority"] = std::numeric_limits<std::uint64_t>::max();
			RequireSchemaFailure(MakeGame(Json::array({ std::move(invalidGroup) })),
				"ResourceWarmup accepted an unsigned priority outside int32.");
			RequireSchemaFailure(
				MakeGame(Json::array({ MakeGroup("Bad:Name", { "Assets/Prefabs/A.hprefab" }) })),
				"ResourceWarmup accepted ':' in a group identifier.");

			RequireInvalidArgument([&]
			{
				(void)BuildRuntimeWarmupPlan(
					MakeGame(Json::array({ MakeGroup("Missing", { "Assets/Prefabs/Missing.hprefab" }) })),
					RuntimeResourceCatalogSnapshot::Empty(),
					projectRoot);
			}, "Loose warm-up planning accepted a missing root file.");
			RequireInvalidArgument([&]
			{
				(void)BuildRuntimeWarmupPlan(
					MakeGame(Json::array({ MakeGroup("Unsupported", { "Assets/Data.txt" }) })),
					RuntimeResourceCatalogSnapshot::Empty(),
					projectRoot);
			}, "Loose warm-up planning accepted an unsupported explicit root.");

			RuntimeCatalogDependency materialDependency;
			materialDependency.Path = "Assets/Materials/A.hmat";
			materialDependency.Kind = RuntimeCatalogDependencyKind::HardRuntime;
			materialDependency.Source = RuntimeCatalogDependencySource::Structured;
			const auto packagedSnapshot = MakeSnapshot({
				MakeCatalogEntry(
					"Assets/Prefabs/A.hprefab",
					RuntimeCatalogResourceType::Prefab,
					{ materialDependency },
					{ "Warmup:Pack" }),
				MakeCatalogEntry(
					"Assets/Materials/A.hmat",
					RuntimeCatalogResourceType::Material,
					{},
					{ "Warmup:Pack" })
			});
			const RuntimeResourceWarmupPlan packaged = BuildRuntimeWarmupPlan(
				MakeGame(Json::array({ MakeGroup("Pack", { "Assets/Prefabs/A.hprefab" }) })),
				packagedSnapshot,
				projectRoot);
			Require(packaged.bPackaged && packaged.Groups.front().Targets.size() == 2,
				"Packaged warm-up planning did not consume the tagged closure.");
			const auto explicitTarget = std::find_if(
				packaged.Groups.front().Targets.begin(),
				packaged.Groups.front().Targets.end(),
				[](const RuntimeResourceWarmupTarget& target)
				{
					return target.bExplicitRoot;
				});
			const auto skippedTarget = std::find_if(
				packaged.Groups.front().Targets.begin(),
				packaged.Groups.front().Targets.end(),
				[](const RuntimeResourceWarmupTarget& target)
				{
					return !target.bExplicitRoot && target.Type == RuntimeCatalogResourceType::Material;
				});
			Require(explicitTarget != packaged.Groups.front().Targets.end(),
				"Packaged warm-up planning lost the explicit root marker.");
			Require(skippedTarget != packaged.Groups.front().Targets.end(),
				"Packaged warm-up planning did not preserve unsupported closure entries as skipped targets.");

			RequireInvalidArgument([&]
			{
				const auto staleSnapshot = MakeSnapshot({
					MakeCatalogEntry(
						"Assets/Prefabs/A.hprefab",
						RuntimeCatalogResourceType::Prefab,
						{},
						{})
				});
				(void)BuildRuntimeWarmupPlan(
					MakeGame(Json::array({ MakeGroup("Pack", { "Assets/Prefabs/A.hprefab" }) })),
					staleSnapshot,
					projectRoot);
			}, "Packaged warm-up planning accepted an explicit root without its group tag.");
			RequireInvalidArgument([&]
			{
				const auto staleClosure = MakeSnapshot({
					MakeCatalogEntry(
						"Assets/Prefabs/A.hprefab",
						RuntimeCatalogResourceType::Prefab,
						{},
						{ "Warmup:Pack" }),
					MakeCatalogEntry(
						"Assets/Materials/A.hmat",
						RuntimeCatalogResourceType::Material,
						{},
						{ "Warmup:Pack" })
				});
				(void)BuildRuntimeWarmupPlan(
					MakeGame(Json::array({ MakeGroup("Pack", { "Assets/Prefabs/A.hprefab" }) })),
					staleClosure,
					projectRoot);
			}, "Packaged warm-up planning accepted a stale tagged closure.");
		}

		void RuntimeContract(const std::filesystem::path& projectRoot)
		{
			RuntimeResourceRegistry registry;
			registry.Init(2);
			RuntimeResourceWarmupManager manager;
			try
			{
				const Json game = MakeGame(Json::array({
					MakeGroup("High", { "Assets/Prefabs/A.hprefab", "Assets/Prefabs/B.hprefab" }, "All", 10, "Startup"),
					MakeGroup("Low", { "Assets/Prefabs/C.hprefab" }, "All", 0)
				}));
				const RuntimeResourceWarmupPlan plan = BuildRuntimeWarmupPlan(
					game,
					RuntimeResourceCatalogSnapshot::Empty(),
					projectRoot);
				RuntimeResourceWarmupManager missingDispatch;
				RequireInvalidArgument([&]
				{
					missingDispatch.Initialize(registry, plan, {});
				}, "Warm-up manager accepted an empty dispatch callback.");
				manager.Initialize(
					registry,
					plan,
					MakeHeadlessWarmupDispatch(registry, projectRoot));
				Require(manager.QueueTrigger("All") == 2,
					"Warm-up trigger did not queue both configured groups.");

				manager.Pump();
				auto high = manager.GetGroupProgress("High");
				auto low = manager.GetGroupProgress("Low");
				Require(high && low && high->Total == 2 && high->Dispatched == 1 &&
					high->Undispatched == 1 && low->Dispatched == 0,
					"A warm-up Pump dispatched more than one target or ignored priority.");
				manager.Pump();
				high = manager.GetGroupProgress("High");
				low = manager.GetGroupProgress("Low");
				Require(high && low && high->Dispatched == 2 && low->Dispatched == 0,
					"Warm-up priority did not drain the higher group first.");
				manager.Pump();
				low = manager.GetGroupProgress("Low");
				Require(low && low->Dispatched == 1,
					"Warm-up Pump did not dispatch the next priority group.");

				PumpUntilTerminal(manager, registry, "High");
				PumpUntilTerminal(manager, registry, "Low");
				high = manager.GetGroupProgress("High");
				low = manager.GetGroupProgress("Low");
				Require(high && low && high->State == RuntimeResourceWarmupGroupState::Ready &&
					low->State == RuntimeResourceWarmupGroupState::Ready &&
					high->Ratio == 1.0f && low->Ratio == 1.0f,
					"Warm-up groups did not reach stable Ready progress.");
				Require(registry.GetStats().PhysicalLoads == 3,
					"Warm-up alias/dedup physical load count was not deterministic.");
				Require(registry.CollectUnused() == 0,
					"Ready warm-up resources were not retained by group pins.");

				Require(manager.ReleaseResidency(RuntimeResourceWarmupResidency::Startup) == 1,
					"Startup residency did not release exactly one group.");
				Require(registry.CollectUnused() == 2,
					"Released Startup resources were not reclaimed.");
				Require(manager.ReleaseGroup("Low"),
					"Session warm-up group was not released.");
				Require(registry.CollectUnused() == 1 && registry.GetStats().Resident == 0,
					"Released Session resources were not reclaimed.");
			}
			catch (...)
			{
				manager.Shutdown();
				registry.Shutdown();
				throw;
			}
			manager.Shutdown();
			registry.Shutdown();
		}

		void FailureAndSharedReleaseContract(const std::filesystem::path& projectRoot)
		{
			RuntimeResourceRegistry registry;
			registry.Init(1);
			RuntimeResourceWarmupManager manager;
			try
			{
				const Json game = MakeGame(Json::array({
					MakeGroup("Bad", { "Assets/Prefabs/Bad.hprefab" }),
					MakeGroup("Shared", { "Assets/Prefabs/Shared.hprefab" })
				}));
				manager.Initialize(
					registry,
					BuildRuntimeWarmupPlan(
						game,
						RuntimeResourceCatalogSnapshot::Empty(),
						projectRoot),
					MakeHeadlessWarmupDispatch(registry, projectRoot));

				Require(manager.QueueGroup("Bad"), "Bad warm-up group was not queued.");
				PumpUntilTerminal(manager, registry, "Bad");
				const auto bad = manager.GetGroupProgress("Bad");
				Require(bad && bad->State == RuntimeResourceWarmupGroupState::Failed && bad->Failed == 1,
					"Bad prefab did not fail its warm-up group.");
				bool bRethrewFailure = false;
				try
				{
					manager.RethrowFailure("Bad");
				}
				catch (const std::exception&)
				{
					bRethrewFailure = true;
				}
				Require(bRethrewFailure, "Warm-up manager did not rethrow its stored failure.");

				const RuntimeResourceWarmupTarget sharedTarget{
					"Assets/Prefabs/Shared.hprefab",
					RuntimeCatalogResourceType::Prefab,
					true
				};
				RuntimeResourceGroup owner = registry.CreateGroup();
				(void)QueueHeadlessWarmupTarget(registry, projectRoot, sharedTarget, owner);
				Require(registry.GetStats().PhysicalLoads == 2,
					"Shared prefab did not start one physical load.");
				Require(manager.QueueGroup("Shared"), "Shared warm-up group was not queued.");
				manager.Pump();
				Require(manager.ReleaseGroup("Shared"), "Loading warm-up group was not released.");

				RuntimeResourceHandle<HeadlessWarmupResource> shared =
					AcquireHeadlessWarmupTarget(registry, projectRoot, sharedTarget);
				Require(shared && shared->Document.at("Name") == "Shared",
					"Releasing a warm-up join invalidated the shared generation.");
				Require(registry.GetStats().PhysicalLoads == 2,
					"Shared warm-up join created a second physical load.");
				shared.Reset();
				owner.Release();
				(void)registry.CollectUnused();
			}
			catch (...)
			{
				manager.Shutdown();
				registry.Shutdown();
				throw;
			}
			manager.Shutdown();
			registry.Shutdown();
		}

		void InjectedMeshContract(const std::filesystem::path& projectRoot)
		{
			RuntimeResourceRegistry registry;
			registry.Init(1);
			RuntimeResourceWarmupManager manager;
			try
			{
				manager.Initialize(
					registry,
					BuildRuntimeWarmupPlan(
						MakeGame(Json::array({
							MakeGroup("Mesh", { "Assets/Models/Invalid.hmesh" })
						})),
						RuntimeResourceCatalogSnapshot::Empty(),
						projectRoot),
					MakeHeadlessWarmupDispatch(registry, projectRoot));
				Require(manager.QueueGroup("Mesh"),
					"An injected mesh warm-up group was not queued.");
				manager.Pump();
				const auto dispatched = manager.GetGroupProgress("Mesh");
				Require(dispatched && dispatched->Dispatched == 1,
					"Mesh warm-up did not dispatch through the provided registry and root.");

				PumpUntilTerminal(manager, registry, "Mesh");
				const auto failed = manager.GetGroupProgress("Mesh");
				Require(failed && failed->State == RuntimeResourceWarmupGroupState::Failed,
					"An invalid injected mesh did not fail through its provided registry.");
				bool bRethrewFailure = false;
				try
				{
					manager.RethrowFailure("Mesh");
				}
				catch (const std::exception&)
				{
					bRethrewFailure = true;
				}
				Require(bRethrewFailure,
					"Injected mesh warm-up did not retain its decode failure.");
			}
			catch (...)
			{
				manager.Shutdown();
				registry.Shutdown();
				throw;
			}
			manager.Shutdown();
			registry.Shutdown();
		}

		void ProjectServiceContract(const std::filesystem::path& projectRoot)
		{
			const std::filesystem::path configPath = projectRoot / "Config" / "Game.json";
			const std::filesystem::path engineContentRoot =
				projectRoot.parent_path() / "Engine" / "Content";
			std::filesystem::create_directories(engineContentRoot);
			RuntimeResourceRegistry registry;
			registry.Init(1);
			RuntimeProjectResourceWarmup warmup;
			try
			{
				Utils::SaveJsonToFile(Json{
					{ "Game", {
						{ "DefaultScene", "Assets/Scenes/Main.he" }
					} }
				}, configPath);
				warmup.Initialize(
					registry,
					projectRoot,
					engineContentRoot,
					"Config/Game.json",
					RuntimeResourceCatalogSnapshot::Empty());
				Require(warmup.GetDefaultScene() == "Assets/Scenes/Main.he",
					"Project warm-up service did not retain Game.DefaultScene.");
				Require(!warmup.QueueGroup("Boot"),
					"An omitted optional ResourceWarmup object produced a Boot group.");
				warmup.Pump();
				warmup.Shutdown();

				Json game = MakeGame(Json::array({
					MakeGroup("Boot", { "Assets/Prefabs/Shared.hprefab" }, "Boot", 100, "Startup")
				}));
				game["DefaultScene"] = "Assets/Scenes/Main.he";
				Utils::SaveJsonToFile(Json{ { "Game", std::move(game) } }, configPath);
				warmup.Initialize(
					registry,
					projectRoot,
					engineContentRoot,
					"Config/Game.json",
					RuntimeResourceCatalogSnapshot::Empty());
				Require(warmup.QueueGroup("Boot"),
					"Project warm-up service did not queue its Boot group.");
				PumpProjectWarmupUntilTerminal(warmup, registry, "Boot");
				const auto first = warmup.GetGroupProgress("Boot");
				Require(first && first->State == RuntimeResourceWarmupGroupState::Ready,
					"Project warm-up Boot group did not reach Ready.");
				Require(warmup.ReleaseResidency(RuntimeResourceWarmupResidency::Startup) == 1,
					"Project warm-up did not release its Startup residency.");
				(void)registry.CollectUnused();

				Require(warmup.QueueGroup("Boot"),
					"Released project warm-up Boot group could not be queued for a second play.");
				PumpProjectWarmupUntilTerminal(warmup, registry, "Boot");
				const auto second = warmup.GetGroupProgress("Boot");
				Require(second && second->State == RuntimeResourceWarmupGroupState::Ready,
					"Project warm-up Boot group did not become Ready on its second play.");
				(void)warmup.ReleaseResidency(RuntimeResourceWarmupResidency::Startup);
				(void)registry.CollectUnused();
			}
			catch (...)
			{
				warmup.Shutdown();
				registry.Shutdown();
				throw;
			}
			warmup.Shutdown();
			registry.Shutdown();
		}
	}

	void RuntimeResourceWarmupContract()
	{
		ScopedTempDirectory tempDirectory;
		const std::filesystem::path projectRoot = tempDirectory.Get() / "Project";
		const std::filesystem::path prefabDirectory = projectRoot / "Assets" / "Prefabs";
		const std::filesystem::path modelDirectory = projectRoot / "Assets" / "Models";
		const std::filesystem::path configDirectory = projectRoot / "Config";
		std::filesystem::create_directories(prefabDirectory);
		std::filesystem::create_directories(modelDirectory);
		std::filesystem::create_directories(configDirectory);
		Utils::SaveJsonToFile(Json{ { "Name", "A" } }, prefabDirectory / "A.hprefab");
		Utils::SaveJsonToFile(Json{ { "Name", "B" } }, prefabDirectory / "B.hprefab");
		Utils::SaveJsonToFile(Json{ { "Name", "C" } }, prefabDirectory / "C.hprefab");
		Utils::SaveJsonToFile(Json{ { "Name", "Shared" } }, prefabDirectory / "Shared.hprefab");
		{
			std::ofstream bad(prefabDirectory / "Bad.hprefab", std::ios::binary | std::ios::trunc);
			bad << "{";
		}
		{
			std::ofstream data(projectRoot / "Assets" / "Data.txt", std::ios::binary | std::ios::trunc);
			data << "unsupported";
		}
		{
			std::ofstream invalidMesh(
				modelDirectory / "Invalid.hmesh",
				std::ios::binary | std::ios::trunc);
			invalidMesh << "{";
		}

		PlanContract(projectRoot);
		RuntimeContract(projectRoot);
		FailureAndSharedReleaseContract(projectRoot);
		InjectedMeshContract(projectRoot);
		ProjectServiceContract(projectRoot);
	}
}
