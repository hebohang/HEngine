#include "Hepch.h"

#include "Runtime/UnitTest/ConfigManagerTests.h"

#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
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
                static std::atomic_uint64_t sSuffix = 0;
                const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
                mPath = std::filesystem::temp_directory_path() /
                    ("HEngine_ConfigManager_" + std::to_string(timestamp) + "_" +
                        std::to_string(sSuffix.fetch_add(1, std::memory_order_relaxed)));
                std::filesystem::create_directories(mPath);
            }

            ~ScopedTempDirectory()
            {
                std::error_code error;
                std::filesystem::remove_all(mPath, error);
            }

            [[nodiscard]] const std::filesystem::path& GetPath() const noexcept
            {
                return mPath;
            }

        private:
            std::filesystem::path mPath;
        };

        class ScopedConfigManagerReset final
        {
        public:
            ScopedConfigManagerReset()
            {
                ConfigManager::GetInstance().Clear();
            }

            ~ScopedConfigManagerReset()
            {
                ConfigManager::GetInstance().Clear();
            }
        };

        void WriteText(const std::filesystem::path& path, std::string_view contents)
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            Require(file.is_open(), "Could not create a ConfigManager test fixture.");
            file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            file.close();
            Require(!file.fail(), "Could not write a ConfigManager test fixture.");
        }

        [[nodiscard]] std::filesystem::path NormalizeExistingPath(
            const std::filesystem::path& path)
        {
            std::error_code error;
            const std::filesystem::path normalized = std::filesystem::weakly_canonical(path, error);
            Require(!error, "Could not normalize a ConfigManager test fixture path.");
            return normalized;
        }
    }

    void ConfigManagerDualRootContract()
    {
        ScopedTempDirectory temporaryDirectory;
        ScopedConfigManagerReset configReset;

        const std::filesystem::path projectRoot = temporaryDirectory.GetPath() / "Project";
        const std::filesystem::path engineContentRoot =
            temporaryDirectory.GetPath() / "Engine" / "Content";
        const std::filesystem::path savedRoot = temporaryDirectory.GetPath() / "UserSaved";
        const std::filesystem::path generatedContentRoot =
            temporaryDirectory.GetPath() / "Build" / "GeneratedContent" / "Engine";

        WriteText(projectRoot / "Project.heproject", "{}\n");
        WriteText(
            projectRoot / "Config" / "BaseEngine.json",
            "{\"Renderer\":{\"API\":\"DX12\",\"VSync\":true}}\n");
        WriteText(projectRoot / "Config" / "Game.json", "{\"Game\":{}}\n");
        WriteText(projectRoot / "Config" / "BaseEditor.json", "{}\n");
        WriteText(engineContentRoot / "Config" / "BaseEditor.json", "{}\n");
        std::filesystem::create_directories(projectRoot / "Assets");
        std::filesystem::create_directories(projectRoot / "Shaders");
        std::filesystem::create_directories(projectRoot / "Scripts");
        std::filesystem::create_directories(engineContentRoot);
        std::filesystem::create_directories(generatedContentRoot / "Shaders" / "CACHES");

        const std::filesystem::path projectOverride =
            projectRoot / "Assets" / "Shared" / "Override.txt";
        const std::filesystem::path engineOverride =
            engineContentRoot / "Assets" / "Shared" / "Override.txt";
        const std::filesystem::path engineOnly =
            engineContentRoot / "Shaders" / "BuiltIn.hlsl";
        const std::filesystem::path engineOwnedAsset =
            engineContentRoot / "Assets" / "Shared" / "EngineOwned.asset";
        WriteText(projectOverride, "project\n");
        WriteText(engineOverride, "engine\n");
        WriteText(engineOnly, "engine-only\n");
        WriteText(engineOwnedAsset, "engine-original\n");

        ConfigManager::GetInstance().Init(
            projectRoot / "Project.heproject",
            engineContentRoot,
            savedRoot,
            generatedContentRoot);

        Require(
            ConfigManager::GetFullPath("Assets/Shared/Override.txt") ==
                NormalizeExistingPath(projectOverride),
            "Project content did not override an engine content file at the same path.");
        Require(
            ConfigManager::GetFullPath("Shaders/BuiltIn.hlsl") ==
                NormalizeExistingPath(engineOnly),
            "A missing project file did not fall back to engine content.");
        Require(
            ConfigManager::GetContentRootForPath(engineOnly) ==
                NormalizeExistingPath(engineContentRoot),
            "An engine-owned asset did not retain its physical content root.");

        const std::filesystem::path missingProjectFile =
            NormalizeExistingPath(projectRoot) / "Assets" / "Generated" / "New.asset";
        Require(
            ConfigManager::GetFullPath("Assets/Generated/New.asset") == missingProjectFile,
            "A missing content path was not kept in the writable project root.");

        Require(
            ConfigManager::GetEngineContentFullPath("Shaders/BuiltIn.hlsl") ==
                NormalizeExistingPath(engineOnly),
            "An explicit engine content lookup resolved outside the engine root.");
        Require(
            ConfigManager::GetEngineGeneratedContentFullPath("Shaders/CACHES") ==
                NormalizeExistingPath(generatedContentRoot) / "Shaders" / "CACHES",
            "An explicit generated-content lookup resolved outside the build root.");
        Require(
            ConfigManager::GetProjectFullPath("Shaders/BuiltIn.hlsl") ==
                NormalizeExistingPath(projectRoot) / "Shaders" / "BuiltIn.hlsl",
            "An explicit project write path fell back into engine content.");

        const Json projectAsset = { { "Owner", "project" } };
        Require(
            AssetManager::SaveAsset(
                projectAsset,
                std::string("Assets/Shared/EngineOwned.asset")),
            "AssetManager could not save a project override for an engine-owned asset.");
        Require(
            Utils::LoadJsonFile(projectRoot / "Assets" / "Shared" / "EngineOwned.asset") == projectAsset,
            "AssetManager did not write the override into the project root.");
        std::ifstream engineAssetFile(engineOwnedAsset, std::ios::binary);
        const std::string engineAssetContents{
            std::istreambuf_iterator<char>(engineAssetFile),
            std::istreambuf_iterator<char>() };
        Require(
            engineAssetContents == "engine-original\n",
            "AssetManager overwrote the engine fallback asset.");

        Require(
            std::filesystem::path(ConfigManager::GetRelativePath(projectOverride)) ==
                std::filesystem::path("Assets/Shared/Override.txt"),
            "A project file did not round-trip to a project-relative path.");
        Require(
            std::filesystem::path(ConfigManager::GetRelativePath(engineOnly)) ==
                std::filesystem::path("Shaders/BuiltIn.hlsl"),
            "An engine file did not round-trip to an engine-content-relative path.");

        Require(
            ConfigManager::GetInstance().GetSavedFolder() == savedRoot.lexically_normal(),
            "The explicit Saved root was not retained.");
        Require(
            std::filesystem::is_directory(savedRoot),
            "The explicit Saved root was not created.");
        Require(
            ConfigManager::GetSavedPath("Editor/Layout.ini") ==
                savedRoot / "Editor" / "Layout.ini",
            "A Saved-relative path resolved outside the explicit Saved root.");

        bool rejectedProjectTraversal = false;
        try
        {
            (void)ConfigManager::GetFullPath("../outside.asset");
        }
        catch (const std::invalid_argument&)
        {
            rejectedProjectTraversal = true;
        }
        Require(rejectedProjectTraversal, "A project-relative parent traversal was accepted.");

        bool rejectedEngineTraversal = false;
        try
        {
            (void)ConfigManager::GetEngineContentFullPath("../outside.asset");
        }
        catch (const std::invalid_argument&)
        {
            rejectedEngineTraversal = true;
        }
        Require(rejectedEngineTraversal, "An engine-relative parent traversal was accepted.");
    }
}
