#include "Hepch.h"

#include "Runtime/UnitTest/SystemLifecycleTests.h"

#include "Runtime/EcsFramework/Component/Script/PythonScriptComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/EcsFramework/System/Physics/PhysicSystem3D.h"
#include "Runtime/EcsFramework/System/Script/PythonScriptSystem.h"
#include "Runtime/EcsFramework/System/SystemLifecycle.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Scripting/ScriptEngine.h"

#include <HEngine/ProjectBuildConfig.h>

#include <pybind11/embed.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine::Tests
{
	namespace
	{
		class LifecycleFailure final : public std::runtime_error
		{
		public:
			explicit LifecycleFailure(int code)
				: std::runtime_error("Lifecycle failure " + std::to_string(code)),
				  Code(code)
			{
			}

			int Code = 0;
		};

		struct FailureReport
		{
			std::string Context;
			int Code = 0;
		};

		void Require(bool condition, std::string_view message)
		{
			if (!condition)
				throw std::runtime_error(std::string(message));
		}

		void RequireSequence(
			const std::vector<int>& actual,
			std::initializer_list<int> expected,
			std::string_view message)
		{
			Require(
				actual == std::vector<int>(expected),
				message);
		}

		void RecordFailure(
			std::vector<FailureReport>& reports,
			std::string_view context,
			const std::exception_ptr& failure)
		{
			try
			{
				std::rethrow_exception(failure);
			}
			catch (const LifecycleFailure& error)
			{
				reports.push_back({ std::string(context), error.Code });
			}
			catch (...)
			{
				reports.push_back({ std::string(context), -1 });
			}
		}

		void VerifyStartupRollbackIncludesCurrentSystem()
		{
			size_t activeSystemCount = 0;
			std::vector<int> events;
			std::vector<FailureReport> reports;
			events.reserve(5);
			reports.reserve(2);

			int primaryFailureCode = 0;
			try
			{
				SystemLifecycle::Start(
					3,
					activeSystemCount,
					[&events](size_t systemIndex)
					{
						events.push_back(100 + static_cast<int>(systemIndex));
						if (systemIndex == 1)
							throw LifecycleFailure(11);
					},
					[&events](size_t systemIndex)
					{
						events.push_back(200 + static_cast<int>(systemIndex));
						if (systemIndex == 1)
							throw LifecycleFailure(21);
					},
					[&events]
					{
						events.push_back(300);
						throw LifecycleFailure(31);
					},
					[&reports](
						std::string_view context,
						const std::exception_ptr& failure)
					{
						RecordFailure(reports, context, failure);
					});
			}
			catch (const LifecycleFailure& error)
			{
				primaryFailureCode = error.Code;
			}

			Require(primaryFailureCode == 11,
				"Startup rollback replaced the original start failure.");
			Require(activeSystemCount == 0,
				"Startup rollback left a runtime system marked active.");
			RequireSequence(
				events,
				{ 100, 101, 201, 200, 300 },
				"Startup rollback did not stop current and prior systems in reverse order.");
			Require(reports.size() == 2 && reports[0].Code == 21 && reports[1].Code == 31,
				"Startup rollback did not report all secondary cleanup failures.");
		}

		void VerifyActiveStartIsRejectedWithoutMutation()
		{
			size_t activeSystemCount = 0;
			std::vector<int> events;
			SystemLifecycle::Start(
				2,
				activeSystemCount,
				[&events](size_t systemIndex)
				{
					events.push_back(static_cast<int>(systemIndex));
				},
				[](size_t) {},
				[] {},
				[](std::string_view, const std::exception_ptr&) {});

			bool bCallbackCalled = false;
			bool bRejected = false;
			try
			{
				SystemLifecycle::Start(
					3,
					activeSystemCount,
					[&bCallbackCalled](size_t) { bCallbackCalled = true; },
					[&bCallbackCalled](size_t) { bCallbackCalled = true; },
					[&bCallbackCalled] { bCallbackCalled = true; },
					[&bCallbackCalled](std::string_view, const std::exception_ptr&)
					{
						bCallbackCalled = true;
					});
			}
			catch (const std::logic_error&)
			{
				bRejected = true;
			}

			Require(bRejected, "Starting an active lifecycle was not rejected.");
			Require(activeSystemCount == 2,
				"Rejected lifecycle startup changed the active prefix.");
			RequireSequence(events, { 0, 1 },
				"Rejected lifecycle startup changed prior startup events.");
			Require(!bCallbackCalled,
				"Rejected lifecycle startup invoked a lifecycle callback.");
		}

		void VerifySuccessfulReverseStopAndRepeatedStop()
		{
			size_t activeSystemCount = 0;
			std::vector<int> events;

			SystemLifecycle::Start(
				3,
				activeSystemCount,
				[&events](size_t systemIndex)
				{
					events.push_back(100 + static_cast<int>(systemIndex));
				},
				[](size_t) {},
				[] {},
				[](std::string_view, const std::exception_ptr&) {});
			Require(activeSystemCount == 3,
				"Successful startup did not retain the full active prefix.");
			RequireSequence(events, { 100, 101, 102 },
				"Successful startup did not visit systems in forward order.");

			events.clear();
			SystemLifecycle::Stop(
				activeSystemCount,
				[&events](size_t systemIndex)
				{
					events.push_back(200 + static_cast<int>(systemIndex));
				},
				[&events] { events.push_back(300); },
				[](std::string_view, const std::exception_ptr&) {});
			Require(activeSystemCount == 0,
				"Successful stop did not clear the active prefix.");
			RequireSequence(events, { 202, 201, 200, 300 },
				"Successful stop did not use reverse order before cleanup.");

			events.clear();
			SystemLifecycle::Stop(
				activeSystemCount,
				[&events](size_t systemIndex)
				{
					events.push_back(200 + static_cast<int>(systemIndex));
				},
				[&events] { events.push_back(300); },
				[](std::string_view, const std::exception_ptr&) {});
			RequireSequence(events, { 300 },
				"Repeated stop invoked a runtime system more than once.");
		}

		void VerifyStopFailuresPreserveFirstAndClearAllSystems()
		{
			size_t activeSystemCount = 3;
			std::vector<int> events;
			std::vector<FailureReport> reports;
			events.reserve(4);
			reports.reserve(3);

			int primaryFailureCode = 0;
			try
			{
				SystemLifecycle::Stop(
					activeSystemCount,
					[&events](size_t systemIndex)
					{
						events.push_back(200 + static_cast<int>(systemIndex));
						throw LifecycleFailure(40 + static_cast<int>(systemIndex));
					},
					[&events]
					{
						events.push_back(300);
						throw LifecycleFailure(50);
					},
					[&reports](
						std::string_view context,
						const std::exception_ptr& failure)
					{
						RecordFailure(reports, context, failure);
					});
			}
			catch (const LifecycleFailure& error)
			{
				primaryFailureCode = error.Code;
			}

			Require(primaryFailureCode == 42,
				"Stop replaced the first reverse-order system failure.");
			Require(activeSystemCount == 0,
				"Failed stop did not clear the active prefix before callbacks.");
			RequireSequence(events, { 202, 201, 200, 300 },
				"Failed stop skipped a system or final cleanup.");
			Require(
				reports.size() == 3 &&
				reports[0].Code == 41 &&
				reports[1].Code == 40 &&
				reports[2].Code == 50,
				"Failed stop did not report every failure after the first one.");
		}

		void VerifyCleanupFailureBecomesPrimary()
		{
			size_t activeSystemCount = 1;
			int primaryFailureCode = 0;
			try
			{
				SystemLifecycle::Stop(
					activeSystemCount,
					[](size_t) {},
					[] { throw LifecycleFailure(60); },
					[](std::string_view, const std::exception_ptr&) {});
			}
			catch (const LifecycleFailure& error)
			{
				primaryFailureCode = error.Code;
			}

			Require(primaryFailureCode == 60,
				"Cleanup failure was not propagated when system stops succeeded.");
			Require(activeSystemCount == 0,
				"Cleanup failure restored an already cleared active prefix.");
		}

		void VerifyPhysicSystem3DRuntimeCanRestart()
		{
			Level level;
			PhysicSystem3D physicsSystem(&level);
			Require(physicsSystem.GetPhysicsSystem() == nullptr,
				"A new 3D physics system unexpectedly owned runtime state.");

			for (int cycle = 0; cycle < 2; ++cycle)
			{
				physicsSystem.OnRuntiemStart();
				Require(physicsSystem.GetPhysicsSystem() != nullptr,
					"3D physics startup did not publish its runtime system.");

				physicsSystem.OnRuntimeStop();
				Require(physicsSystem.GetPhysicsSystem() == nullptr,
					"3D physics stop did not release its runtime system.");
			}

			physicsSystem.OnRuntimeStop();
			Require(physicsSystem.GetPhysicsSystem() == nullptr,
				"Repeated 3D physics stop recreated or retained runtime state.");
		}

		void VerifyPythonLifecycleStateWithoutOwners()
		{
			Level level;
			Level otherLevel;
			PythonScriptSystem scriptSystem(&level);
			PythonScriptSystem sameLevelSystem(&level);
			PythonScriptSystem otherLevelSystem(&otherLevel);
			scriptSystem.OnRuntiemStart();

			bool bRepeatedStartRejected = false;
			try
			{
				scriptSystem.OnRuntiemStart();
			}
			catch (const std::logic_error&)
			{
				bRepeatedStartRejected = true;
			}
			Require(bRepeatedStartRejected,
				"Python runtime accepted a repeated start when no owners were registered.");

			for (PythonScriptSystem* competingSystem : { &sameLevelSystem, &otherLevelSystem })
			{
				bool bCompetingStartRejected = false;
				try
				{
					competingSystem->OnRuntiemStart();
				}
				catch (const std::logic_error&)
				{
					bCompetingStartRejected = true;
				}
				Require(bCompetingStartRejected,
					"A competing Python runtime system replaced the active owner.");

				competingSystem->OnRuntimeStop();
				Require(PythonScriptSystem::GetActiveLevel() == &level,
					"A rejected Python runtime system cleared the active owner.");
			}

			scriptSystem.OnRuntimeStop();
			Require(PythonScriptSystem::GetActiveLevel() == nullptr,
				"Owner-free Python runtime stop retained the active level.");

			scriptSystem.OnRuntiemStart();
			scriptSystem.OnRuntimeStop();
			scriptSystem.OnRuntimeStop();
			Require(PythonScriptSystem::GetActiveLevel() == nullptr,
				"Repeated owner-free Python runtime stop changed global lifecycle state.");
		}

		void VerifyDestroyedPythonOwnerIsUnregistered()
		{
			const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
			const std::string moduleName = "he_lifecycle_fixture_" + std::to_string(suffix);
			const std::string failureModuleName = moduleName + "_failure";
			const std::filesystem::path fixtureParent =
				std::filesystem::temp_directory_path() /
				std::filesystem::path(L"HEngine-\u8def\u5f84-\u6d4b\u8bd5");
			const std::filesystem::path fixtureRoot = fixtureParent / moduleName;
			const std::filesystem::path fixtureDirectory = fixtureRoot / "Scripts";
			const std::filesystem::path savedDirectory =
				fixtureRoot / std::filesystem::path(L"Saved-\u7528\u6237-\u6570\u636e");
			std::filesystem::create_directories(fixtureDirectory);
			std::filesystem::create_directories(fixtureRoot / "Config");

			const auto cleanupFixture = [&fixtureParent, &fixtureRoot]() noexcept
			{
				std::error_code cleanupError;
				std::filesystem::remove_all(fixtureRoot, cleanupError);
				cleanupError.clear();
				(void)std::filesystem::remove(fixtureParent, cleanupError);
			};

			try
			{
				std::ofstream baseEngineConfig(fixtureRoot / "Config" / "BaseEngine.json");
				Require(baseEngineConfig.is_open(),
					"Failed to create the Unicode Python path fixture config.");
				baseEngineConfig << R"({"Renderer":{"API":"DX12","VSync":false}})";
				baseEngineConfig.close();
				Require(!baseEngineConfig.fail(),
					"Failed to write the Unicode Python path fixture config.");

				std::ofstream fixture(fixtureDirectory / (moduleName + ".py"));
				Require(fixture.is_open(), "Failed to create the Python lifecycle fixture.");
				fixture << "import HEngine\n\n"
					"class " << moduleName << ":\n"
					"    def Start(self, entity):\n"
					"        self.entity = entity\n"
					"    def Stop(self):\n"
					"        HEngine.Destroy(self.entity)\n";
				fixture.close();
				Require(!fixture.fail(), "Failed to write the Python lifecycle fixture.");

				std::ofstream failureFixture(
					fixtureDirectory / (failureModuleName + ".py"));
				Require(failureFixture.is_open(),
					"Failed to create the failing Python lifecycle fixture.");
				failureFixture << "stop_count = 0\n\n"
					"class " << failureModuleName << ":\n"
					"    def Start(self, entity):\n"
					"        raise RuntimeError('expected lifecycle failure')\n"
					"    def Stop(self):\n"
					"        global stop_count\n"
					"        stop_count += 1\n";
				failureFixture.close();
				Require(!failureFixture.fail(),
					"Failed to write the failing Python lifecycle fixture.");

				ConfigManager::GetInstance().Init(
					fixtureRoot,
					std::filesystem::path(HE_ENGINE_CONTENT_DIR),
					savedDirectory,
					{},
					"Config/BaseEngine.json");
				try
				{
					ScriptEngine::GetInstance().Init();
					try
					{
						pybind11::module_ engineModule =
							pybind11::module_::import("HEngine");
						const std::filesystem::path expectedProjectPath =
							ConfigManager::GetFullPath("Config/BaseEngine.json");
						const std::filesystem::path expectedSavedPath =
							ConfigManager::GetSavedPath("Config/Unicode.json");
						Require(
							engineModule.attr("GetFullPath")("Config/BaseEngine.json")
								.cast<std::wstring>() == expectedProjectPath.wstring(),
							"Python GetFullPath did not preserve a Unicode Windows path.");
						Require(
							engineModule.attr("GetSavedPath")("Config/Unicode.json")
								.cast<std::wstring>() == expectedSavedPath.wstring(),
							"Python GetSavedPath did not preserve a Unicode Windows path.");

						pybind11::module_ failureModule =
							pybind11::module_::import(failureModuleName.c_str());

						Level level;
						PythonScriptSystem* scriptSystem = level.TryGetSystem<PythonScriptSystem>();
						Require(scriptSystem != nullptr,
							"The lifecycle fixture level has no Python script system.");
						bool bRuntimeStarted = false;
						try
						{
							scriptSystem->OnRuntiemStart();
							bRuntimeStarted = true;

							Entity entity = level.CreateEntity("PythonLifecycleOwner");
							const UUID owner = entity.UUID();
							entity.AddComponent<PythonScriptComponent>(moduleName + ".py");
							Require(!ScriptEngine::IsModuleRegistered(owner) &&
								!ScriptEngine::IsClassRegistered(owner),
								"Adding a Python component created an unstarted module generation.");

							const UUID outerOwner;
							PythonScriptSystem::SetCurrentScriptOwner(outerOwner);
							Require(scriptSystem->StartRuntimeOwner(entity),
								"A dynamic Python owner could not enter the active runtime.");
							const UUID* restoredOwner = PythonScriptSystem::GetCurrentScriptOwner();
							Require(restoredOwner != nullptr && *restoredOwner == outerOwner,
								"A nested Python owner did not restore its caller owner.");
							PythonScriptSystem::ClearCurrentScriptOwner();
							Require(ScriptEngine::IsModuleRegistered(owner) &&
								ScriptEngine::IsClassRegistered(owner),
								"A dynamic Python component did not register and start its owner.");

							level.DestroyEntity(entity);
							Require(!ScriptEngine::IsModuleRegistered(owner) &&
								!ScriptEngine::IsClassRegistered(owner),
								"Destroying a dynamic Python owner did not release its module.");

							Entity invalidEntity = level.CreateEntity("InvalidPythonLifecycleOwner");
							const UUID invalidOwner = invalidEntity.UUID();
							invalidEntity.AddComponent<PythonScriptComponent>(
								failureModuleName + "_missing.py");
							bool bInvalidStartRejected = false;
							try
							{
								(void)scriptSystem->StartRuntimeOwner(invalidEntity);
							}
							catch (const std::runtime_error& error)
							{
								bInvalidStartRejected =
									std::string(error.what()).find("No module named") !=
									std::string::npos;
							}
							Require(bInvalidStartRejected,
								"A failed Python module import did not preserve an actionable diagnostic.");
							Require(!ScriptEngine::IsModuleRegistered(invalidOwner) &&
								!ScriptEngine::IsClassRegistered(invalidOwner),
								"A failed Python module import retained partial registration state.");
							level.DestroyEntity(invalidEntity);

							Entity failingEntity = level.CreateEntity("FailingPythonLifecycleOwner");
							const UUID failingOwner = failingEntity.UUID();
							failingEntity.AddComponent<PythonScriptComponent>(
								failureModuleName + ".py");
							bool bStartCallbackFailurePropagated = false;
							const UUID failureOuterOwner;
							PythonScriptSystem::SetCurrentScriptOwner(failureOuterOwner);
							try
							{
								(void)scriptSystem->StartRuntimeOwner(failingEntity);
							}
							catch (const pybind11::error_already_set& error)
							{
								bStartCallbackFailurePropagated =
									std::string(error.what()).find("expected lifecycle failure") !=
									std::string::npos;
							}
							Require(bStartCallbackFailurePropagated,
								"A Python Start callback failure did not preserve its original exception.");
							const UUID* restoredFailureOwner =
								PythonScriptSystem::GetCurrentScriptOwner();
							Require(restoredFailureOwner != nullptr &&
								*restoredFailureOwner == failureOuterOwner,
								"A failed Python Start callback did not restore its caller owner.");
							PythonScriptSystem::ClearCurrentScriptOwner();
							Require(failureModule.attr("stop_count").cast<int>() == 1,
								"A failed Python Start callback did not run rollback Stop exactly once.");
							Require(!ScriptEngine::IsModuleRegistered(failingOwner) &&
								!ScriptEngine::IsClassRegistered(failingOwner),
								"A failed Python Start callback retained registration state.");
							level.DestroyEntity(failingEntity);
							Require(failureModule.attr("stop_count").cast<int>() == 1,
								"Destroying a rolled-back Python owner invoked Stop a second time.");

							scriptSystem->OnRuntimeStop();
							bRuntimeStarted = false;
							Require(!ScriptEngine::IsModuleRegistered(owner) &&
								!ScriptEngine::IsClassRegistered(owner),
								"A destroyed Python owner remained registered after runtime stop.");
							Require(PythonScriptSystem::GetActiveLevel() == nullptr,
								"Python runtime stop retained the active level.");
							Require(failureModule.attr("stop_count").cast<int>() == 1,
								"Runtime stop retained a rolled-back Python owner snapshot.");
						}
						catch (...)
						{
							if (bRuntimeStarted)
							{
								try
								{
									scriptSystem->OnRuntimeStop();
								}
								catch (...)
								{
								}
							}
							throw;
						}
					}
					catch (...)
					{
						ScriptEngine::GetInstance().Shutdown();
						throw;
					}
					ScriptEngine::GetInstance().Shutdown();
				}
				catch (...)
				{
					ConfigManager::GetInstance().Clear();
					throw;
				}
				ConfigManager::GetInstance().Clear();
			}
			catch (...)
			{
				cleanupFixture();
				throw;
			}
			cleanupFixture();
		}
	}

	void SystemLifecycleContract()
	{
		VerifyStartupRollbackIncludesCurrentSystem();
		VerifyActiveStartIsRejectedWithoutMutation();
		VerifySuccessfulReverseStopAndRepeatedStop();
		VerifyStopFailuresPreserveFirstAndClearAllSystems();
		VerifyCleanupFailureBecomesPrimary();
		VerifyPhysicSystem3DRuntimeCanRestart();
		VerifyPythonLifecycleStateWithoutOwners();
		VerifyDestroyedPythonOwnerIsUnregistered();
	}
}
