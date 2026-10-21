#pragma once

#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <cmath>

namespace HEngine
{
    // A proof of absence, not a replica of transient shadow-slot selection.
    // Unknown providers must not inherit the standard raster proof.
    [[nodiscard]] inline bool SsgiHasPotentialShadowTransport(const RenderWorld& world,
        bool bShadowsEnabled, bool bKnownStandardRasterPath) noexcept
    {
        if (!bKnownStandardRasterPath) return true;
        if (!bShadowsEnabled) return false;
        // Local lights have no per-light bCastShadows contract. Their automatic
        // atlas selection can change after a move, so even unselected lights count.
        if (!world.PointLights.empty() || !world.SpotLights.empty()) return true;
        for (const auto& light : world.DirectionalLights)
        {
            if (!light.bCastShadows) continue;
            if (!std::isfinite(light.Intensity) || !std::isfinite(light.ShadowStrength)) return true;
            if (light.Intensity > 0.f && light.ShadowStrength > 0.f) return true;
        }
        return false;
    }
}
