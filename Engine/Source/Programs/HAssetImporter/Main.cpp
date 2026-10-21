#include "Runtime/Core/Log/Log.h"
#include "Runtime/Core/Modules/ModuleManager.h"
#include "Runtime/Core/Plugins/Services/MeshImporterService.h"
#include "Runtime/Core/Plugins/Services/SceneImporterService.h"
#include "Runtime/EcsFramework/Serializer/SceneDocument.h"
#include "Runtime/Graphics/Mesh/MeshResourceValidator.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "_generated/serializer/all_serializer.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <HEngine/ProjectBuildConfig.h>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <unordered_set>

namespace HEngine
{
    void RegisterStaticPluginsForTools(PluginManager& pluginManager);
}

namespace
{
    constexpr int kInvalidArgumentsExitCode = 2;
    constexpr int kImportFailedExitCode = 3;
    constexpr int kUnexpectedFailureExitCode = 4;

    [[nodiscard]] constexpr HEngine::PluginConfiguration GetPluginConfiguration()
    {
#if defined(HE_ASSET_IMPORTER_PLUGIN_CONFIG_DEBUG)
        return HEngine::PluginConfiguration::Debug;
#elif defined(HE_ASSET_IMPORTER_PLUGIN_CONFIG_RELWITHDEBINFO)
        return HEngine::PluginConfiguration::RelWithDebInfo;
#elif defined(HE_ASSET_IMPORTER_PLUGIN_CONFIG_MINSIZEREL)
        return HEngine::PluginConfiguration::MinSizeRel;
#else
        return HEngine::PluginConfiguration::Release;
#endif
    }

    void LogUsage()
    {
        HE_CORE_ERROR("Usage: HAssetImporter <project-root> <source-model> <output-dir>");
    }

    bool IsPathWithin(const std::filesystem::path& path, const std::filesystem::path& root)
    {
        std::error_code ec;
        const std::filesystem::path relativePath = std::filesystem::relative(path, root, ec);
        if (ec || relativePath.empty() || relativePath.is_absolute())
            return false;

        for (const std::filesystem::path& component : relativePath)
        {
            if (component == "..")
                return false;
        }
        return true;
    }

    // The legacy mesh importer writes files as it goes. Keep failed imports from
    // leaving partial native resources in an otherwise valid Assets directory.
    // Existing files are never touched by this rollback.
    class NewOutputFilesRollback
    {
    public:
        explicit NewOutputFilesRollback(const std::filesystem::path& directory)
            : mDirectory(directory)
        {
            for (const auto& entry : std::filesystem::directory_iterator(mDirectory))
                mExistingNames.insert(entry.path().filename());
        }

        ~NewOutputFilesRollback()
        {
            if (mCommitted)
                return;
            std::error_code scanError;
            std::filesystem::directory_iterator it(mDirectory, scanError);
            if (scanError)
                return;
            const std::filesystem::directory_iterator end;
            while (it != end)
            {
                const auto& entry = *it;
                if (mExistingNames.contains(entry.path().filename()))
                {
                    it.increment(scanError);
                    if (scanError) break;
                    continue;
                }
                std::error_code statusError;
                if (entry.is_regular_file(statusError) && !statusError)
                {
                    std::error_code removeError;
                    std::filesystem::remove(entry.path(), removeError);
                    if (removeError)
                        HE_CORE_WARN("HAssetImporter: could not roll back '{0}': {1}",
                                     entry.path().string(), removeError.message());
                }
                it.increment(scanError);
                if (scanError) break;
            }
        }

        void Commit() { mCommitted = true; }

    private:
        std::filesystem::path mDirectory;
        std::unordered_set<std::filesystem::path> mExistingNames;
        bool mCommitted = false;
    };
}

int main(int argc, char* argv[])
{
    HEngine::Log::Init();

    const bool sceneMode = argc == 5 && std::string(argv[4]) == "--scene";
    const bool validateMode = argc == 5 && std::string(argv[4]) == "--validate-scene";
    if (argc != 4 && !sceneMode && !validateMode)
    {
        LogUsage();
        return kInvalidArgumentsExitCode;
    }

    HEngine::ModuleManager& moduleManager = HEngine::ModuleManager::GetInstance();
    bool bPluginHostConfigured = false;
    int result = kUnexpectedFailureExitCode;
    try
    {
        const std::filesystem::path projectRoot = argv[1];
        const std::filesystem::path sourceModel = argv[2];
        const std::filesystem::path outputDir = argv[3];

        if (!projectRoot.is_absolute() || !sourceModel.is_absolute() || !outputDir.is_absolute())
        {
            HE_CORE_ERROR("HAssetImporter: all paths must be absolute");
            return kInvalidArgumentsExitCode;
        }

        if (!std::filesystem::is_directory(projectRoot))
        {
            HE_CORE_ERROR("HAssetImporter: project root does not exist or is not a directory: '{0}'", projectRoot.string());
            return kInvalidArgumentsExitCode;
        }
        if (!std::filesystem::is_regular_file(sourceModel))
        {
            HE_CORE_ERROR("HAssetImporter: source model does not exist or is not a file: '{0}'", sourceModel.string());
            return kInvalidArgumentsExitCode;
        }
        if (!std::filesystem::is_directory(outputDir))
        {
            HE_CORE_ERROR("HAssetImporter: output directory does not exist or is not a directory: '{0}'", outputDir.string());
            return kInvalidArgumentsExitCode;
        }

        const std::filesystem::path canonicalProjectRoot = std::filesystem::canonical(projectRoot);
        const std::filesystem::path canonicalSourceModel = std::filesystem::canonical(sourceModel);
        const std::filesystem::path canonicalOutputDir = std::filesystem::canonical(outputDir);
        const std::filesystem::path assetsRoot = canonicalProjectRoot / "Assets";
        const std::filesystem::path engineAssetsRoot =
            std::filesystem::canonical(std::filesystem::path(HE_ENGINE_CONTENT_DIR) / "Assets");
        const std::filesystem::path baseEngineConfig = canonicalProjectRoot / "Config" / "BaseEngine.json";
        const std::filesystem::path defaultEngineConfig =
            std::filesystem::path(HE_ENGINE_CONTENT_DIR) / "Config" / "DefaultEngine.json";

        if (!std::filesystem::is_directory(assetsRoot) ||
            !std::filesystem::is_regular_file(baseEngineConfig) ||
            !std::filesystem::is_regular_file(defaultEngineConfig))
        {
            HE_CORE_ERROR(
                "HAssetImporter: project root must contain Assets and Config/BaseEngine.json, and Engine content must contain Config/DefaultEngine.json: project='{0}', engine='{1}'",
                canonicalProjectRoot.string(),
                std::filesystem::path(HE_ENGINE_CONTENT_DIR).string());
            return kInvalidArgumentsExitCode;
        }

        const std::filesystem::path canonicalAssetsRoot = std::filesystem::canonical(assetsRoot);
        if (!(sceneMode && IsPathWithin(canonicalOutputDir, canonicalProjectRoot / ".he-content")) &&
            !IsPathWithin(canonicalOutputDir, canonicalAssetsRoot) &&
            !IsPathWithin(canonicalOutputDir, engineAssetsRoot))
        {
            HE_CORE_ERROR(
                "HAssetImporter: output directory must be inside project or engine Assets ('{0}', '{1}'): '{2}'",
                canonicalAssetsRoot.string(),
                engineAssetsRoot.string(),
                canonicalOutputDir.string());
            return kInvalidArgumentsExitCode;
        }

        HEngine::ConfigManager::GetInstance().Init(
            canonicalProjectRoot,
            HE_ENGINE_CONTENT_DIR,
            std::filesystem::path{},
            std::filesystem::path{},
            HE_PROJECT_GAME_CONFIG);

        if (validateMode)
        {
            const auto document = HEngine::ParseSceneDocument(HEngine::ResolveSceneDocumentPath(
                canonicalProjectRoot, std::filesystem::relative(canonicalSourceModel, canonicalProjectRoot).generic_string()));
            for (const auto& path : document.GetMeshPaths())
            {
                HEngine::MeshRes mesh;
                HEngine::Serializer::Deserialize(HEngine::Utils::LoadJsonFile(canonicalProjectRoot / path), mesh);
                const auto validation = HEngine::MeshResourceValidator::LoadAndValidateBuffers(mesh, canonicalProjectRoot);
                if (!validation) throw std::runtime_error(validation.Message);
            }
            return EXIT_SUCCESS;
        }

        moduleManager.ConfigurePluginHost(HEngine::PluginHostEnvironment{
            .Host = HEngine::PluginHost::Tools,
            .BuildDistribution = HEngine::PluginBuildDistribution::Development,
            .Platform = HEngine::GetCurrentPluginPlatform(),
            .Configuration = GetPluginConfiguration(),
        });
        bPluginHostConfigured = true;
        HEngine::RegisterStaticPluginsForTools(moduleManager.GetPluginManager());
        moduleManager.StartPluginsThroughPhase(HEngine::PluginLoadPhase::PreDevice);
        moduleManager.StartPluginsThroughPhase(HEngine::PluginLoadPhase::PostDevice);
        moduleManager.StartPluginsThroughPhase(HEngine::PluginLoadPhase::PostEngine);

        if (sceneMode)
        {
            const auto importer = moduleManager.GetPluginManager().GetServices()
                .TryGet<HEngine::ISceneImporterService>(HEngine::c_SceneImporterServiceId);
            if (!importer) throw std::runtime_error("Scene importer service is unavailable.");
            const auto converted = importer->Convert(canonicalSourceModel, canonicalOutputDir);
            HEngine::Utils::SaveJsonToFile(converted, canonicalOutputDir / "conversion.json");
            result = converted.value("ok", false) ? EXIT_SUCCESS : kImportFailedExitCode;
        }
        else
        {
        const HEngine::Ref<HEngine::IMeshImporterService> importer =
            moduleManager.GetPluginManager().GetServices()
                .TryGet<HEngine::IMeshImporterService>(
                    HEngine::c_MeshImporterServiceId);
        if (!importer)
        {
            HE_CORE_ERROR(
                "HAssetImporter: no mesh importer service is available for the Tools host");
            result = kImportFailedExitCode;
        }
        else
        {
            NewOutputFilesRollback rollback(canonicalOutputDir);
            if (!importer->Import(canonicalSourceModel, canonicalOutputDir))
            {
                HE_CORE_ERROR("HAssetImporter: import failed for '{0}'", canonicalSourceModel.string());
                result = kImportFailedExitCode;
            }
            else
            {
                rollback.Commit();
                HE_CORE_INFO(
                    "HAssetImporter: imported '{0}' into '{1}'",
                    canonicalSourceModel.string(),
                    canonicalOutputDir.string());
                result = EXIT_SUCCESS;
            }
        }
        }
    }
    catch (const std::exception& exception)
    {
        HE_CORE_ERROR("HAssetImporter: unexpected failure: {0}", exception.what());
        result = kUnexpectedFailureExitCode;
    }
    catch (...)
    {
        HE_CORE_ERROR("HAssetImporter: unexpected non-standard failure");
        result = kUnexpectedFailureExitCode;
    }

    if (bPluginHostConfigured)
    {
        try
        {
            moduleManager.StopPlugins();
        }
        catch (const std::exception& exception)
        {
            HE_CORE_ERROR(
                "HAssetImporter: plugin shutdown failed: {0}",
                exception.what());
            result = kUnexpectedFailureExitCode;
        }
        catch (...)
        {
            HE_CORE_ERROR("HAssetImporter: plugin shutdown failed with a non-standard exception");
            result = kUnexpectedFailureExitCode;
        }
    }
    return result;
}
