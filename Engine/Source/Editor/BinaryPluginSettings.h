#pragma once

#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

namespace HEngine::BinaryPluginSettings
{
    inline bool OfflineRendererEnabled()
    {
        const auto overridePath = ConfigManager::GetSavedPath("Config/EditorPlugins.json");
        if (std::filesystem::is_regular_file(overridePath))
            return Utils::LoadJsonFile(overridePath).value("OfflineReferenceRenderer", false);
        const auto projectPath = ConfigManager::GetProjectFullPath("Config/Plugins.json");
        if (!std::filesystem::is_regular_file(projectPath)) return false;
        const auto plugins = Utils::LoadJsonFile(projectPath);
        return plugins.contains("he.offline-reference-renderer") &&
            plugins.at("he.offline-reference-renderer").value("Editor", false);
    }

    inline void SetOfflineRendererEnabled(bool enabled)
    {
        const auto path = ConfigManager::GetSavedPath("Config/EditorPlugins.json");
        auto settings = std::filesystem::is_regular_file(path) ? Utils::LoadJsonFile(path) : Json::object();
        settings["OfflineReferenceRenderer"] = enabled;
        std::filesystem::create_directories(path.parent_path());
        Utils::SaveJsonToFile(settings, path);
    }
}
