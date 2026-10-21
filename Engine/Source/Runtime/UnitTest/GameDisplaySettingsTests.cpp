#include "Hepch.h"
#include "Runtime/UnitTest/GameDisplaySettingsTests.h"
#include "Runtime/Resource/ConfigManager/GameDisplaySettings.h"

#include <stdexcept>

namespace HEngine::Tests
{
    void GameDisplaySettingsContract()
    {
        const Json empty = Json::object();
        const Json windowed = { { "Fullscreen", false } };
        const Json fullscreen = { { "Fullscreen", true } };
        if (ParseGameDisplaySettings(empty).bFullscreen)
            throw std::runtime_error("An unconfigured project was forced fullscreen.");
        const auto settings = ParseGameDisplaySettings(Json{
            { "Game", { { "Display", { { "DefaultMode", "BorderlessFullscreen" },
                { "PreferencesPath", "Config/PlayerSettings.json" } } } } } });
        if (!settings.bFullscreen || settings.PreferencesPath != "Config/PlayerSettings.json")
            throw std::runtime_error("Project startup display defaults were lost.");
        if (!ResolveGameFullscreen(true, empty, empty, std::nullopt, false) ||
            ResolveGameFullscreen(true, fullscreen, windowed, std::nullopt, false) ||
            !ResolveGameFullscreen(false, windowed, fullscreen, std::nullopt, false))
            throw std::runtime_error("Saved display choice did not override project defaults, including explicit false.");
        if (ResolveGameFullscreen(true, fullscreen, fullscreen, false, false) ||
            !ResolveGameFullscreen(false, windowed, windowed, true, false) ||
            ResolveGameFullscreen(true, fullscreen, fullscreen, true, true))
            throw std::runtime_error("CLI mode or fixed-resolution window precedence changed.");
        for (const char* path : { "../outside.json", "C:/absolute.json", "Assets/preferences.json" })
        {
            bool rejected = false;
            try { (void)ParseGameDisplaySettings(Json{ { "Game", { { "Display", { { "PreferencesPath", path } } } } } }); }
            catch (const std::invalid_argument&) { rejected = true; }
            if (!rejected) throw std::runtime_error("Unsafe display preference path was accepted.");
        }
        bool badMode = false;
        try { (void)ParseGameDisplaySettings(Json{ { "Game", { { "Display", { { "DefaultMode", "Exclusive" } } } } } }); }
        catch (const std::invalid_argument&) { badMode = true; }
        if (!badMode) throw std::runtime_error("Unsupported exclusive display mode was silently accepted.");
    }
}
