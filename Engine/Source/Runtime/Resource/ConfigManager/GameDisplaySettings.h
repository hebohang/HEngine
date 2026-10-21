#pragma once

#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <optional>
#include <stdexcept>
#include <string>

namespace HEngine
{
    struct GameDisplaySettings
    {
        bool bFullscreen = false;
        std::string PreferencesPath;
    };

    [[nodiscard]] inline GameDisplaySettings ParseGameDisplaySettings(const Json& gameConfig)
    {
        GameDisplaySettings result;
        const auto game = gameConfig.find("Game");
        if (game == gameConfig.end() || !game->contains("Display"))
            return result;
        const auto& display = game->at("Display");
        if (!display.is_object())
            throw std::invalid_argument("Game.Display must be an object.");
        const auto mode = display.value("DefaultMode", std::string("Windowed"));
        if (mode != "Windowed" && mode != "BorderlessFullscreen")
            throw std::invalid_argument("Game.Display.DefaultMode must be Windowed or BorderlessFullscreen.");
        result.bFullscreen = mode == "BorderlessFullscreen";
        const auto path = display.value("PreferencesPath", std::string{});
        if (!path.empty())
        {
            const auto validation = Utils::TryNormalizeProjectRelativePath(path, result.PreferencesPath);
            if (!validation || !result.PreferencesPath.starts_with("Config/") || !result.PreferencesPath.ends_with(".json"))
                throw std::invalid_argument("Game.Display.PreferencesPath requires a Config-relative JSON path.");
        }
        return result;
    }

    [[nodiscard]] inline bool ResolveGameFullscreen(bool defaultFullscreen,
        const Json& projectPreferences, const Json& savedPreferences,
        std::optional<bool> commandLineOverride, bool bFixedWindowSize)
    {
        bool fullscreen = defaultFullscreen;
        for (const Json* preferences : { &projectPreferences, &savedPreferences })
        {
            if (preferences->is_object() && preferences->contains("Fullscreen"))
            {
                const auto& value = preferences->at("Fullscreen");
                if (!value.is_boolean())
                    throw std::invalid_argument("Display preference Fullscreen must be boolean.");
                fullscreen = value.get<bool>();
            }
        }
        return bFixedWindowSize ? false : commandLineOverride.value_or(fullscreen);
    }

    // Evaluate once before window creation. A project chooses the preference
    // path; the engine never knows a particular game's frontend file name.
    [[nodiscard]] inline bool LoadGameStartupFullscreen(std::optional<bool> commandLineOverride,
        bool bFixedWindowSize)
    {
        const auto& config = ConfigManager::GetInstance();
        const auto gamePath = config.GetRootFolder() / config.GetGameConfigPath();
        const auto settings = ParseGameDisplaySettings(Utils::LoadJsonFile(gamePath,
            { 1024 * 1024, 32, 65536 }).Value);
        Json projectPreferences = Json::object();
        Json savedPreferences = Json::object();
        if (!settings.PreferencesPath.empty())
        {
            const auto projectPath = ConfigManager::GetProjectFullPath(settings.PreferencesPath);
            const auto savedPath = ConfigManager::GetSavedPath(settings.PreferencesPath);
            if (std::filesystem::exists(projectPath))
                projectPreferences = Utils::LoadJsonFile(projectPath, { 65536, 16, 4096 }).Value;
            if (std::filesystem::exists(savedPath))
                savedPreferences = Utils::LoadJsonFile(savedPath, { 65536, 16, 4096 }).Value;
        }
        return ResolveGameFullscreen(settings.bFullscreen, projectPreferences, savedPreferences,
            commandLineOverride, bFixedWindowSize);
    }
}
