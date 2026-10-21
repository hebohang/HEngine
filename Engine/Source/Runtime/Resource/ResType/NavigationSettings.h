#pragma once

#include "Runtime/Core/Meta/Reflection/Reflection.h"

#include <string>
#include <vector>

namespace HEngine
{
    REFLECTION_TYPE(NavigationSurfaceRes)
    STRUCT(NavigationSurfaceRes, Fields)
    {
        REFLECTION_BODY(NavigationSurfaceRes);
        std::string TagPrefix;
        int Area = 0;
        bool UseMesh = true;
    };

    REFLECTION_TYPE(NavigationBuildSettings)
    STRUCT(NavigationBuildSettings, Fields)
    {
        REFLECTION_BODY(NavigationBuildSettings);
        int Version = 1;
        float CellSize = 0.2f;
        float CellHeight = 0.2f;
        float AgentHeight = 1.8f;
        float AgentRadius = 0.4f;
        float AgentMaxClimb = 0.9f;
        float AgentMaxSlope = 45.0f;
        int MaxGridCells = 4 * 1024 * 1024;
        // Empty preserves NavGeometry (mesh) and NavGround (legacy flat proxy).
        // Explicit rules use first matching prefix. Area 63 is blocked.
        std::vector<NavigationSurfaceRes> Surfaces;
        std::vector<float> AreaCosts;
    };
}
