#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace HEngine
{
    enum class CloudQuality : uint8_t { Off, Low, Balanced, High, Custom };

    struct CloudSettings
    {
        CloudQuality Quality = CloudQuality::Off;
        float RenderScale = 0.5f;
        uint32_t MaxSteps = 40;
        uint32_t LightSteps = 3;
        float HistoryWeight = 0.85f;
        float DetailDistance = 1800.0f;
        bool bTemporal = true;
        uint32_t DebugView = 0;
        bool operator==(const CloudSettings&) const = default;
    };

    [[nodiscard]] inline CloudSettings ResolveCloudSettings(CloudSettings value)
    {
        if (value.Quality > CloudQuality::Custom || !std::isfinite(value.RenderScale) ||
            value.RenderScale < 0.25f || value.RenderScale > 1.0f || value.MaxSteps < 16 ||
            value.MaxSteps > 96 || value.LightSteps > 6 || !std::isfinite(value.HistoryWeight) ||
            value.HistoryWeight < 0.0f || value.HistoryWeight > 0.95f ||
            !std::isfinite(value.DetailDistance) || value.DetailDistance < 100.0f ||
            value.DetailDistance > 20000.0f || value.DebugView > 3)
            throw std::invalid_argument("Invalid volumetric cloud quality settings");
        if (value.Quality == CloudQuality::Low)
        { value.RenderScale = 0.25f; value.MaxSteps = 24; value.LightSteps = 1; }
        if (value.Quality == CloudQuality::Balanced)
        { value.RenderScale = 0.5f; value.MaxSteps = 40; value.LightSteps = 3; }
        if (value.Quality == CloudQuality::High)
        { value.RenderScale = 0.5f; value.MaxSteps = 80; value.LightSteps = 5; }
        return value;
    }

    // Immutable, normalized scene art snapshot. No device resources or ECS references.
    struct RenderCloudLayer
    {
        bool bEnabled = false;
        float Bottom = 90.0f;
        float Thickness = 95.0f;
        float MaxDistance = 5000.0f;
        float Coverage = 0.42f;
        float Density = 0.035f;
        float Scale = 260.0f;
        float Roundness = 0.75f;
        float Erosion = 0.12f;
        uint32_t Seed = 7301;
        float WindAngle = 25.0f;
        float WindSpeed = 1.8f;
        float EvolutionSpeed = 0.015f;
        glm::vec3 LitColor{ 1.0f, 0.97f, 0.9f };
        glm::vec3 ShadowColor{ 0.56f, 0.66f, 0.87f };
        float Ambient = 0.65f;
        float SilverLining = 0.3f;
        bool operator==(const RenderCloudLayer&) const = default;
    };

    inline void NormalizeCloudLayer(RenderCloudLayer& value)
    {
        auto bounded = [](float x, float fallback, float low, float high)
        { return std::isfinite(x) ? std::clamp(x, low, high) : fallback; };
        value.Bottom = bounded(value.Bottom, 90.0f, -10000.0f, 10000.0f);
        value.Thickness = bounded(value.Thickness, 95.0f, 1.0f, 2000.0f);
        value.MaxDistance = bounded(value.MaxDistance, 5000.0f, 10.0f, 20000.0f);
        value.Coverage = bounded(value.Coverage, 0.42f, 0.0f, 1.0f);
        value.Density = bounded(value.Density, 0.035f, 0.0f, 0.2f);
        value.Scale = bounded(value.Scale, 260.0f, 10.0f, 4000.0f);
        value.Roundness = bounded(value.Roundness, 0.75f, 0.0f, 1.0f);
        value.Erosion = bounded(value.Erosion, 0.12f, 0.0f, 0.5f);
        value.WindAngle = bounded(value.WindAngle, 25.0f, -360.0f, 360.0f);
        value.WindSpeed = bounded(value.WindSpeed, 1.8f, 0.0f, 100.0f);
        value.EvolutionSpeed = bounded(value.EvolutionSpeed, 0.015f, 0.0f, 0.5f);
        value.Ambient = bounded(value.Ambient, 0.65f, 0.0f, 2.0f);
        value.SilverLining = bounded(value.SilverLining, 0.3f, 0.0f, 2.0f);
        for (int i = 0; i < 3; ++i)
        {
            value.LitColor[i] = bounded(value.LitColor[i], 1.0f, 0.0f, 4.0f);
            value.ShadowColor[i] = bounded(value.ShadowColor[i], 0.65f, 0.0f, 4.0f);
        }
    }

    // Normal-Z geometry clipping is applied separately; sky has no finite endpoint.
    [[nodiscard]] inline glm::vec2 IntersectCloudLayer(float originY, float directionY,
        const RenderCloudLayer& layer)
    {
        if (std::abs(directionY) < 1e-6f)
            return originY >= layer.Bottom && originY <= layer.Bottom + layer.Thickness
                ? glm::vec2(0.0f, layer.MaxDistance) : glm::vec2(0.0f);
        const float a = (layer.Bottom - originY) / directionY;
        const float b = (layer.Bottom + layer.Thickness - originY) / directionY;
        const float start = std::max(0.0f, std::min(a, b));
        return { start, std::max(start, std::min(layer.MaxDistance, std::max(a, b))) };
    }
}
