#include "Hepch.h"

#include "NavigationService.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "_generated/serializer/NavigationSettings.gen.h"

#include <cmath>
#include <stdexcept>

namespace HEngine
{
    void NavigationService::ValidateSettings(const NavigationBuildSettings& settings)
    {
        const auto range = [](float value, float minimum, float maximum) {
            return std::isfinite(value) && value >= minimum && value <= maximum;
        };
        if (settings.Version != 1 || !range(settings.CellSize, .01f, 10) || !range(settings.CellHeight, .01f, 10) ||
            !range(settings.AgentHeight, .1f, 100) || !range(settings.AgentRadius, 0, 100) ||
            !range(settings.AgentMaxClimb, 0, 100) || !range(settings.AgentMaxSlope, 0, 89) ||
            settings.MaxGridCells < 1 || settings.MaxGridCells > 16 * 1024 * 1024 || settings.Surfaces.size() > 128 ||
            (!settings.AreaCosts.empty() && settings.AreaCosts.size() != 64))
            throw std::invalid_argument("Invalid navigation settings, grid budget or area cost count (expected 64).");
        for (const auto& surface : settings.Surfaces)
            if (surface.TagPrefix.empty() || surface.TagPrefix.size() > 128 || surface.Area < 0 || surface.Area > 63)
                throw std::invalid_argument("Navigation surfaces require a bounded nonempty prefix and area 0..63.");
        for (float cost : settings.AreaCosts)
            if (!range(cost, 1, 1000000)) throw std::invalid_argument("Navigation costs must be finite and in [1, 1000000].");
    }

    NavigationBuildSettings NavigationService::LoadProjectSettings()
    {
        NavigationBuildSettings settings;
        const auto& config = ConfigManager::GetInstance();
        if (config.IsInitialized())
        {
            const auto path = config.GetRootFolder() / "Config/Navigation.json";
            if (std::filesystem::exists(path))
            {
                if (std::filesystem::file_size(path) > 1024 * 1024) throw std::invalid_argument("Navigation settings exceed 1 MiB.");
                settings = Utils::LoadJsonFile(path).get<NavigationBuildSettings>();
            }
        }
        ValidateSettings(settings);
        return settings;
    }
}
