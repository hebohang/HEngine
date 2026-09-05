#pragma once

#include "Runtime/Core/Templates/PublicSingleton.h"
#include "Runtime/Graphics/RHI/RenderBackend.h"

#include <filesystem>
#include <glm/glm.hpp>

namespace HEngine
{
    enum class Language : std::uint8_t
    {
        English = 0,
        Chinese
    };

    class ConfigManager final : public PublicSingleton<ConfigManager>
    {
    public:
        void Init(
            const std::filesystem::path& projectRoot,
            const std::filesystem::path& engineContentRoot,
            const std::filesystem::path& savedRoot = {},
            const std::filesystem::path& engineGeneratedContentRoot = {},
            const std::filesystem::path& gameConfigPath = "Config/Game.json");

        ConfigManager();
        ConfigManager(const ConfigManager&) = delete;
        ConfigManager& operator=(const ConfigManager&) = delete;
    public:
        void Clear();
		[[nodiscard]] bool IsInitialized() const noexcept { return !mRootFolder.empty(); }

        [[nodiscard]] static glm::vec2& GetViewportSize() { return ConfigManager::mViewportSize; }
        [[nodiscard]] static float GetViewportWidth() { return ConfigManager::mViewportSize.x; }
        [[nodiscard]] static float GetViewportHeight() { return ConfigManager::mViewportSize.y; }

        [[nodiscard]] static std::filesystem::path GetFullPath(const std::string& relativePath);
        [[nodiscard]] static std::filesystem::path GetProjectFullPath(const std::string& relativePath);
        [[nodiscard]] static std::filesystem::path GetEngineContentFullPath(const std::string& relativePath);
        [[nodiscard]] static std::filesystem::path GetEngineGeneratedContentFullPath(const std::string& relativePath);
        [[nodiscard]] static std::filesystem::path GetSavedPath(const std::string& relativePath);
        [[nodiscard]] static std::filesystem::path GetContentRootForPath(
            const std::filesystem::path& fullPath);
        [[nodiscard]] static std::string GetRelativePath(const std::filesystem::path& fullPath);

        [[nodiscard]] const std::filesystem::path& GetRootFolder() const;
        [[nodiscard]] const std::filesystem::path& GetEngineContentFolder() const;
        [[nodiscard]] const std::filesystem::path& GetEngineGeneratedContentFolder() const;
        [[nodiscard]] const std::filesystem::path& GetGameConfigPath() const;
        [[nodiscard]] const std::filesystem::path& GetAssetsFolder() const;
        [[nodiscard]] const std::filesystem::path& GetConfigFolder() const;
        [[nodiscard]] const std::filesystem::path& GetShadersFolder() const;
        [[nodiscard]] const std::filesystem::path& GetScriptsFolder() const;
        [[nodiscard]] const std::filesystem::path& GetSavedFolder() const;

#if WITH_EDITOR
        [[nodiscard]] static std::filesystem::path GetEngineFolder()
        {
            return ConfigManager::GetInstance().mEngineContentFolder.parent_path();
        }

        [[nodiscard]] static std::filesystem::path GetEngineFullPath(const std::string& relativePath)
        {
            return ConfigManager::GetInstance().GetEngineFolder() / relativePath;
        }

        [[nodiscard]] static std::filesystem::path GetEditorResourceFullPath(const std::string& resourcePath)
        {
            // example: ConfigManager::GetEditorResourceFullPath(Localization/Chinese.ini)
            return ConfigManager::GetInstance().GetEngineFolder() / "Resources" / resourcePath;
        }
#endif

        void SetLanguage(Language newLanguage);
        [[nodiscard]] static Language GetLanguage() { return ConfigManager::GetInstance().mLanguage; }
        [[nodiscard]] static std::uint8_t GetLanguageIndex() { return static_cast<std::uint8_t>(ConfigManager::GetInstance().mLanguage); }

        // TODO: Move to private
        static glm::vec2 mViewportSize;
        static int selectedEntity;
    private:
        void InitFromBaseEngineConfig();
    public:
        bool VSync = true;
        RenderBackend RendererBackend = RenderBackend::DX12;
    private:
        std::filesystem::path mRootFolder;
        std::filesystem::path mEngineContentFolder;
        std::filesystem::path mEngineGeneratedContentFolder;
        std::filesystem::path mGameConfigPath;
        std::filesystem::path mAssetsFolder;
        std::filesystem::path mConfigFolder;
        std::filesystem::path mShadersFolder;
        std::filesystem::path mScriptsFolder;
        std::filesystem::path mSavedFolder;

        Language mLanguage = Language::Chinese;
    };
}
