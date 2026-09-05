#include "Hepch.h"

#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Graphics/Settings/RenderSettingsService.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "Runtime/ImGui/ImGuiLayer.h"

#include <stdexcept>
#include <vector>

namespace HEngine
{
    namespace
    {
        static bool HasBaseEngineConfig(const std::filesystem::path& rootPath)
        {
            return std::filesystem::exists(rootPath / "Config" / "BaseEngine.json");
        }

        static bool HasProjectDescriptor(const std::filesystem::path& rootPath)
        {
            return std::filesystem::is_regular_file(rootPath / "Project.heproject");
        }

        static bool IsProjectRoot(const std::filesystem::path& rootPath)
        {
            return HasProjectDescriptor(rootPath) || HasBaseEngineConfig(rootPath);
        }

        static std::filesystem::path NormalizePath(const std::filesystem::path& path)
        {
            std::error_code ec;
            std::filesystem::path normalized = std::filesystem::weakly_canonical(path, ec);
            if (!ec)
                return normalized;

            normalized = std::filesystem::absolute(path, ec);
            if (!ec)
                return normalized.lexically_normal();
            return path.lexically_normal();
        }

        static std::filesystem::path ResolveProjectRoot(const std::filesystem::path& requestedRoot)
        {
            if (!requestedRoot.empty())
            {
                const std::filesystem::path requestedPath = requestedRoot.is_absolute()
                    ? requestedRoot
                    : std::filesystem::current_path() / requestedRoot;
                const std::filesystem::path normalized = NormalizePath(requestedPath);
                if (std::filesystem::is_regular_file(normalized) &&
                    normalized.filename() == "Project.heproject")
                {
                    return normalized.parent_path();
                }
                return normalized;
            }

            std::vector<std::filesystem::path> seeds;
            seeds.push_back(std::filesystem::current_path());

            for (const std::filesystem::path& seed : seeds)
            {
                std::filesystem::path current = NormalizePath(seed);
                while (!current.empty())
                {
                    if (IsProjectRoot(current))
                        return current;

                    const std::filesystem::path projectChild = current / "Project";
                    if (IsProjectRoot(projectChild))
                        return projectChild;

                    const std::filesystem::path parent = current.parent_path();
                    if (parent == current)
                        break;
                    current = parent;
                }
            }

            return NormalizePath(std::filesystem::current_path());
        }

        static bool IsWithin(
            const std::filesystem::path& root,
            const std::filesystem::path& candidate)
        {
            const std::filesystem::path relative = candidate.lexically_relative(root);
            if (relative.empty())
                return candidate == root;
            if (relative.is_absolute() || relative.has_root_name())
                return false;
            for (const std::filesystem::path& component : relative)
            {
                if (component == "..")
                    return false;
            }
            return true;
        }

        static std::filesystem::path ResolvePathUnderRoot(
            const std::filesystem::path& root,
            const std::string& relativePath)
        {
            const std::filesystem::path requested = Utils::Utf8ToPath(relativePath);
            if (requested.is_absolute() || requested.has_root_name())
                throw std::invalid_argument("Saved paths must be relative.");
            for (const std::filesystem::path& component : requested)
            {
                if (component == "..")
                    throw std::invalid_argument("Saved paths cannot leave the Saved directory.");
            }

            const std::filesystem::path relative = requested.lexically_normal();
            if (relative.empty() || relative == ".")
                return root;
            return root / relative;
        }
    }

    glm::vec2 ConfigManager::mViewportSize = { 1920.0f, 1080.0f };
    int ConfigManager::selectedEntity = -1;

    void ConfigManager::Init(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& engineContentRoot,
        const std::filesystem::path& savedRoot,
        const std::filesystem::path& engineGeneratedContentRoot,
        const std::filesystem::path& gameConfigPath)
    {
#if defined(HE_PACKAGED_GAME)
        if (!projectRoot.is_absolute() || !engineContentRoot.is_absolute())
            throw std::runtime_error("Packaged game content roots must be absolute.");
        mRootFolder = NormalizePath(projectRoot);
        mEngineContentFolder = NormalizePath(engineContentRoot);
        if (!HasBaseEngineConfig(mRootFolder))
            throw std::runtime_error("Packaged game content is missing Config/BaseEngine.json.");
#else
        mRootFolder = ResolveProjectRoot(projectRoot);
        mEngineContentFolder = NormalizePath(engineContentRoot);
#endif
        if (!IsProjectRoot(mRootFolder))
            throw std::runtime_error("Project root is missing Project.heproject or Config/BaseEngine.json: " + Utils::PathToUtf8(mRootFolder));
        if (!std::filesystem::is_directory(mEngineContentFolder))
            throw std::runtime_error("Engine content root is missing: " + Utils::PathToUtf8(mEngineContentFolder));
        mEngineGeneratedContentFolder = engineGeneratedContentRoot.empty()
            ? mEngineContentFolder
            : NormalizePath(engineGeneratedContentRoot);
        if (!std::filesystem::is_directory(mEngineGeneratedContentFolder))
        {
            throw std::runtime_error(
                "Engine generated content root is missing: " +
                Utils::PathToUtf8(mEngineGeneratedContentFolder));
        }

        mGameConfigPath = gameConfigPath.lexically_normal();
        if (mGameConfigPath.empty() || mGameConfigPath == "." ||
            mGameConfigPath.is_absolute() || mGameConfigPath.has_root_name())
        {
            throw std::invalid_argument("Game config path must be project-relative.");
        }
        for (const std::filesystem::path& component : mGameConfigPath)
        {
            if (component == "..")
                throw std::invalid_argument("Game config path cannot leave the project root.");
        }
        if (!std::filesystem::is_regular_file(mRootFolder / mGameConfigPath))
        {
            throw std::runtime_error(
                "Project game config is missing: " + Utils::PathToUtf8(mRootFolder / mGameConfigPath));
        }
        mAssetsFolder = mRootFolder / "Assets";
        mConfigFolder = mRootFolder / "Config";
        mShadersFolder = mRootFolder / "Shaders";
        mScriptsFolder = mRootFolder / "Scripts";
        mSavedFolder = savedRoot.empty() ? mRootFolder / "Saved" : savedRoot;
        if (!mSavedFolder.is_absolute())
        {
            std::error_code absoluteError;
            const std::filesystem::path absoluteSaved = std::filesystem::absolute(mSavedFolder, absoluteError);
            if (!absoluteError)
                mSavedFolder = absoluteSaved;
        }
        mSavedFolder = mSavedFolder.lexically_normal();

        std::error_code error;
        const std::filesystem::file_status existingStatus = std::filesystem::symlink_status(mSavedFolder, error);
        if (!error && std::filesystem::is_symlink(existingStatus))
            throw std::runtime_error("The Saved directory cannot be a symbolic link.");
        error.clear();
        std::filesystem::create_directories(mSavedFolder, error);
        if (error)
            throw std::runtime_error("Could not create the Saved directory: " + Utils::PathToUtf8(mSavedFolder));
        error.clear();
        const std::filesystem::file_status savedStatus = std::filesystem::symlink_status(mSavedFolder, error);
        if (error || std::filesystem::is_symlink(savedStatus) || !std::filesystem::is_directory(savedStatus))
            throw std::runtime_error("Could not create the Saved directory: " + Utils::PathToUtf8(mSavedFolder));

        InitFromBaseEngineConfig();
    }

    void ConfigManager::InitFromBaseEngineConfig()
    {
        RenderSettingsService& renderSettings =
            RenderSettingsService::GetInstance();
        renderSettings.Initialize({
            .EngineDefaultPath =
                mEngineContentFolder / "Config" / "DefaultEngine.json",
            .ProjectOverridePath =
                mRootFolder / "Config" / "BaseEngine.json",
            .UserOverridePath =
                mSavedFolder / "Config" / "UserEngine.json",
        });
        const ResolvedRenderSettings resolved = renderSettings.GetSnapshot();
        RendererBackend = resolved.Effective.Backend;
        VSync = resolved.Effective.bVSync;
        for (const RenderSettingDiagnostic& diagnostic : resolved.Diagnostics)
        {
            if (diagnostic.Severity ==
                RenderSettingDiagnosticSeverity::Error)
            {
                HE_CORE_ERROR("RenderSettings [{}] {}: {}",
                    diagnostic.PropertyId, diagnostic.Code,
                    diagnostic.Message);
            }
            else if (diagnostic.Severity ==
                RenderSettingDiagnosticSeverity::Warning)
            {
                HE_CORE_WARN("RenderSettings [{}] {}: {}",
                    diagnostic.PropertyId, diagnostic.Code,
                    diagnostic.Message);
            }
            else
            {
                HE_CORE_INFO("RenderSettings [{}] {}: {}",
                    diagnostic.PropertyId, diagnostic.Code,
                    diagnostic.Message);
            }
        }

#ifdef WITH_EDITOR
        Json baseEditorConfigJson = Utils::LoadJsonFile(
            ConfigManager::GetEngineContentFullPath("Config/BaseEditor.json"));
        const std::filesystem::path projectEditorConfig =
            ConfigManager::GetProjectFullPath("Config/BaseEditor.json");
        if (std::filesystem::is_regular_file(projectEditorConfig))
            baseEditorConfigJson.merge_patch(Utils::LoadJsonFile(projectEditorConfig));
#endif

#ifdef WITH_EDITOR
        // init language
        if (baseEditorConfigJson.contains("Language"))
        {
            std::string languageName = baseEditorConfigJson["Language"];
            if (languageName.compare("English") == 0)
            {
                mLanguage = Language::English;
            }
            else if (languageName.compare("Chinese") == 0)
            {
                mLanguage = Language::Chinese;
            }
        }
#endif

    }

    ConfigManager::ConfigManager()
    {
    }

    void ConfigManager::Clear()
    {
        RenderSettingsService::GetInstance().Clear();
        mRootFolder.clear();
        mEngineContentFolder.clear();
        mEngineGeneratedContentFolder.clear();
        mGameConfigPath.clear();
        mAssetsFolder.clear();
        mConfigFolder.clear();
        mShadersFolder.clear();
        mScriptsFolder.clear();
        mSavedFolder.clear();
    }

    std::filesystem::path ConfigManager::GetFullPath(const std::string& relativePath)
    {
        std::filesystem::path requested = Utils::Utf8ToPath(relativePath);
        if (requested.is_absolute() || requested.has_root_name())
        {
#if defined(HE_PACKAGED_GAME)
            throw std::invalid_argument("Packaged content paths must be relative.");
#else
            return NormalizePath(requested);
#endif
        }
        for (const std::filesystem::path& component : requested)
        {
            if (component == "..")
                throw std::invalid_argument("Content paths cannot leave the project or engine content root.");
        }
        requested = requested.lexically_normal();

        const ConfigManager& config = ConfigManager::GetInstance();
        const std::filesystem::path projectPath = config.GetRootFolder() / requested;
        std::error_code error;
        if (std::filesystem::exists(projectPath, error) && !error)
            return projectPath;

        const std::filesystem::path enginePath = config.GetEngineContentFolder() / requested;
        error.clear();
        if (std::filesystem::exists(enginePath, error) && !error)
            return enginePath;

        // Non-existing paths are project-owned so save/create operations can
        // never write into the read-only engine content tree.
        return projectPath;
    }

    std::filesystem::path ConfigManager::GetEngineContentFullPath(const std::string& relativePath)
    {
        std::filesystem::path requested = Utils::Utf8ToPath(relativePath);
        if (requested.is_absolute() || requested.has_root_name())
            throw std::invalid_argument("Engine content paths must be relative.");
        for (const std::filesystem::path& component : requested)
        {
            if (component == "..")
                throw std::invalid_argument("Engine content paths cannot leave the engine content root.");
        }
        requested = requested.lexically_normal();
        return ConfigManager::GetInstance().GetEngineContentFolder() / requested;
    }

    std::filesystem::path ConfigManager::GetEngineGeneratedContentFullPath(
        const std::string& relativePath)
    {
        std::filesystem::path requested = Utils::Utf8ToPath(relativePath);
        if (requested.is_absolute() || requested.has_root_name())
            throw std::invalid_argument("Generated content paths must be relative.");
        for (const std::filesystem::path& component : requested)
        {
            if (component == "..")
                throw std::invalid_argument("Generated content paths cannot leave the generated content root.");
        }
        requested = requested.lexically_normal();
        return ConfigManager::GetInstance().GetEngineGeneratedContentFolder() / requested;
    }

    std::filesystem::path ConfigManager::GetProjectFullPath(const std::string& relativePath)
    {
        std::filesystem::path requested = Utils::Utf8ToPath(relativePath);
        if (requested.is_absolute() || requested.has_root_name())
            throw std::invalid_argument("Project content paths must be relative.");
        for (const std::filesystem::path& component : requested)
        {
            if (component == "..")
                throw std::invalid_argument("Project content paths cannot leave the project root.");
        }
        requested = requested.lexically_normal();
        return ConfigManager::GetInstance().GetRootFolder() / requested;
    }

    std::filesystem::path ConfigManager::GetSavedPath(const std::string& relativePath)
    {
        return ResolvePathUnderRoot(ConfigManager::GetInstance().GetSavedFolder(), relativePath);
    }

    std::filesystem::path ConfigManager::GetContentRootForPath(
        const std::filesystem::path& fullPath)
    {
        const std::filesystem::path normalized = NormalizePath(fullPath);
        const ConfigManager& config = ConfigManager::GetInstance();
        if (IsWithin(config.GetRootFolder(), normalized))
            return config.GetRootFolder();
        if (IsWithin(config.GetEngineContentFolder(), normalized))
            return config.GetEngineContentFolder();
        throw std::invalid_argument("Path is outside the project and engine content roots.");
    }

    std::string ConfigManager::GetRelativePath(const std::filesystem::path& fullPath)
    {
        const std::filesystem::path normalized = NormalizePath(fullPath);
        const ConfigManager& config = ConfigManager::GetInstance();
        if (IsWithin(config.GetRootFolder(), normalized))
            return Utils::PathToUtf8(normalized.lexically_relative(config.GetRootFolder()));
        if (IsWithin(config.GetEngineContentFolder(), normalized))
            return Utils::PathToUtf8(normalized.lexically_relative(config.GetEngineContentFolder()));
        if (IsWithin(config.GetEngineGeneratedContentFolder(), normalized))
            return Utils::PathToUtf8(normalized.lexically_relative(config.GetEngineGeneratedContentFolder()));
        return Utils::PathToUtf8(std::filesystem::relative(normalized, config.GetRootFolder()));
    }

    const std::filesystem::path& ConfigManager::GetRootFolder() const
    {
        HE_CORE_ASSERT(std::filesystem::exists(mRootFolder));
        return mRootFolder;
    }

    const std::filesystem::path& ConfigManager::GetEngineContentFolder() const
    {
        HE_CORE_ASSERT(std::filesystem::exists(mEngineContentFolder));
        return mEngineContentFolder;
    }

    const std::filesystem::path& ConfigManager::GetEngineGeneratedContentFolder() const
    {
        HE_CORE_ASSERT(std::filesystem::exists(mEngineGeneratedContentFolder));
        return mEngineGeneratedContentFolder;
    }

    const std::filesystem::path& ConfigManager::GetGameConfigPath() const
    {
        return mGameConfigPath;
    }

    const std::filesystem::path& ConfigManager::GetAssetsFolder() const
    { 
        HE_CORE_ASSERT(std::filesystem::exists(mAssetsFolder));
        return mAssetsFolder;
    }

    const std::filesystem::path& ConfigManager::GetConfigFolder() const
    {
        HE_CORE_ASSERT(std::filesystem::exists(mConfigFolder));
        return mConfigFolder;
    }

    const std::filesystem::path& ConfigManager::GetShadersFolder() const
    {
        return mShadersFolder;
    }

    const std::filesystem::path& ConfigManager::GetScriptsFolder() const
    {
        return mScriptsFolder;
    }

    const std::filesystem::path& ConfigManager::GetSavedFolder() const
    {
        HE_CORE_ASSERT(std::filesystem::exists(mSavedFolder));
        return mSavedFolder;
    }

    void ConfigManager::SetLanguage(Language newLanguage)
    {
        mLanguage = newLanguage;
        //ImGuiLayer::ReloadFont();
    }
}
