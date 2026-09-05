#include "Runtime/Core/Log/Log.h"
#include "Runtime/Core/Modules/ModuleManager.h"
#include "Runtime/Core/Plugins/Services/MeshImporterService.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <HEngine/ProjectBuildConfig.h>

#include <cstdlib>
#include <exception>
#include <filesystem>

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
}

int main(int argc, char* argv[])
{
    HEngine::Log::Init();

    if (argc != 4)
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
        if (!IsPathWithin(canonicalOutputDir, canonicalAssetsRoot) &&
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

        moduleManager.ConfigurePluginHost(HEngine::PluginHostEnvironment{
            .Host = HEngine::PluginHost::Tools,
            .BuildDistribution = HEngine::PluginBuildDistribution::Development,
            .Platform = HEngine::PluginPlatform::Windows,
            .Configuration = GetPluginConfiguration(),
        });
        bPluginHostConfigured = true;
        HEngine::RegisterStaticPluginsForTools(moduleManager.GetPluginManager());
        moduleManager.StartPluginsThroughPhase(HEngine::PluginLoadPhase::PreDevice);
        moduleManager.StartPluginsThroughPhase(HEngine::PluginLoadPhase::PostDevice);
        moduleManager.StartPluginsThroughPhase(HEngine::PluginLoadPhase::PostEngine);

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
        else if (!importer->Import(canonicalSourceModel, canonicalOutputDir))
        {
            HE_CORE_ERROR("HAssetImporter: import failed for '{0}'", canonicalSourceModel.string());
            result = kImportFailedExitCode;
        }
        else
        {
            HE_CORE_INFO(
                "HAssetImporter: imported '{0}' into '{1}'",
                canonicalSourceModel.string(),
                canonicalOutputDir.string());
            result = EXIT_SUCCESS;
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
