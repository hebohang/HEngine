#include "Hepch.h"

#include "Runtime/UnitTest/SurvivorProjectContractTests.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Scripting/ScriptEngine.h"

#include <HEngine/ProjectBuildConfig.h>



#include <atomic>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace HEngine::Tests
{
    namespace
    {
        void RunSurvivorProjectContract(std::string_view contractName)
        {
            static std::atomic_uint64_t sCounter = 0;
            const auto timestamp =
                std::chrono::steady_clock::now().time_since_epoch().count();
            const std::filesystem::path savedDirectory =
                std::filesystem::temp_directory_path() /
                ("HEngine-SurvivorProjectContract-" +
                    std::string(contractName) + "-" +
                    std::to_string(timestamp) + "-" +
                    std::to_string(sCounter.fetch_add(
                        1,
                        std::memory_order_relaxed)));
            std::filesystem::create_directories(savedDirectory);

            const auto cleanup = [&savedDirectory]() noexcept
            {
                std::error_code cleanupError;
                std::filesystem::remove_all(savedDirectory, cleanupError);
            };

            try
            {
                ConfigManager::GetInstance().Init(
                    std::filesystem::path(HE_DEFAULT_PROJECT_DIR),
                    std::filesystem::path(HE_ENGINE_CONTENT_DIR),
                    savedDirectory,
                    {},
                    "Config/BaseEngine.json");
                ScriptEngine::GetInstance().Init();
                try
                {
                    ScriptEngine::Execute("import SurvivorProjectContractTests\nSurvivorProjectContractTests.run_contract('" + std::string(contractName) + "')");
                }
                catch (const std::exception& error)
                {
                    const std::string message = error.what();
                    ScriptEngine::GetInstance().Shutdown();
                    throw std::runtime_error("Survivor Project contract failed: " + message);
                }
                ScriptEngine::GetInstance().Shutdown();
            }
            catch (...)
            {
                ScriptEngine::GetInstance().Shutdown();
                ConfigManager::GetInstance().Clear();
                cleanup();
                throw;
            }
            ConfigManager::GetInstance().Clear();
            cleanup();
        }
    }

    void SurvivorContentCatalogContract()
    {
        RunSurvivorProjectContract("SurvivorContentCatalog");
    }

    void SurvivorRuntimeAssetAuthorityContract()
    {
        RunSurvivorProjectContract("SurvivorRuntimeAssetAuthority");
    }

    void SurvivorChallengeCompositionContract()
    {
        RunSurvivorProjectContract("SurvivorChallengeComposition");
    }

    void SurvivorProgressionMigrationContract()
    {
        RunSurvivorProjectContract("SurvivorProgressionMigration");
    }

    void SurvivorEconomyContract()
    {
        RunSurvivorProjectContract("SurvivorEconomy");
    }

    void SurvivorUnlocksAndPurchasesContract()
    {
        RunSurvivorProjectContract("SurvivorUnlocksAndPurchases");
    }

    void SurvivorUpgradePoolControlContract()
    {
        RunSurvivorProjectContract("SurvivorUpgradePoolControl");
    }

    void SurvivorEvolutionResolverContract()
    {
        RunSurvivorProjectContract("SurvivorEvolutionResolver");
    }

    void SurvivorHubGamepadNavigationContract()
    {
        RunSurvivorProjectContract("SurvivorHubGamepadNavigation");
    }

    void SurvivorCharacterStartStateContract()
    {
        RunSurvivorProjectContract("SurvivorCharacterStartState");
    }

    void SurvivorWeaponLevelZeroContract()
    {
        RunSurvivorProjectContract("SurvivorWeaponLevelZero");
    }

    void SurvivorProjectileBudgetContract()
    {
        RunSurvivorProjectContract("SurvivorProjectileBudget");
    }

    void SurvivorWaveTimelineContract()
    {
        RunSurvivorProjectContract("SurvivorWaveTimeline");
    }

    void SurvivorRunEndTransactionContract()
    {
        RunSurvivorProjectContract("SurvivorRunEndTransaction");
    }

    void SurvivorBalanceHarnessContract()
    {
        RunSurvivorProjectContract("SurvivorBalanceHarness");
    }

    void SurvivorQuestsContract()
    {
        RunSurvivorProjectContract("SurvivorQuests");
    }

    void SurvivorRegionObjectivesContract()
    {
        RunSurvivorProjectContract("SurvivorRegionObjectives");
    }

    void SurvivorWeaponSlotsContract()
    {
        RunSurvivorProjectContract("SurvivorWeaponSlots");
    }

    void SurvivorDomainPowersContract()
    {
        RunSurvivorProjectContract("SurvivorDomainPowers");
    }

    void SurvivorObjectiveBossContract()
    {
        RunSurvivorProjectContract("SurvivorObjectiveBoss");
    }

    void SurvivorRegionSceneResourcesContract()
    {
        RunSurvivorProjectContract("SurvivorRegionSceneResources");
    }

    void SurvivorFixedUpdateContract()
    {
        RunSurvivorProjectContract("SurvivorFixedUpdate");
    }

    void SurvivorProjectileVisualWarmupContract()
    {
        RunSurvivorProjectContract("SurvivorProjectileVisualWarmup");
    }
}
