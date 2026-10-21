#pragma once

#include "Runtime/Graphics/Renderer/RenderResolution.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace HEngine
{
    enum class UpscalingMode : uint8_t { Off, Fsr31 };
    enum class UpscalingQuality : uint8_t { NativeAA, Quality, Balanced, Performance, UltraPerformance };
    enum class UpscalingStatus : uint8_t { Off, Pending, UnsupportedView, Suspended, Active, Failed };

    [[nodiscard]] inline const char* ToString(UpscalingStatus status) noexcept
    {
        switch (status)
        {
        case UpscalingStatus::Off: return "Off";
        case UpscalingStatus::Pending: return "Pending";
        case UpscalingStatus::UnsupportedView: return "UnsupportedView";
        case UpscalingStatus::Suspended: return "Suspended";
        case UpscalingStatus::Active: return "Active";
        case UpscalingStatus::Failed: return "Failed";
        }
        return "Pending";
    }

    struct UpscalingSettings
    {
        UpscalingMode Mode = UpscalingMode::Off;
        UpscalingQuality Quality = UpscalingQuality::Quality;
        float Sharpness = 0.2f;
        [[nodiscard]] bool operator==(const UpscalingSettings&) const = default;
    };

    inline constexpr std::array<const char*, 5> UpscalingQualityNames{
        "NativeAA", "Quality", "Balanced", "Performance", "UltraPerformance"};

    [[nodiscard]] inline const char* ToString(UpscalingMode mode) noexcept
    {
        return mode == UpscalingMode::Fsr31 ? "FSR3.1" : "Off";
    }

    [[nodiscard]] inline const char* ToString(UpscalingQuality quality)
    {
        return UpscalingQualityNames.at(static_cast<size_t>(quality));
    }

    [[nodiscard]] inline UpscalingMode ParseUpscalingMode(std::string_view name)
    {
        if (name == "Off") return UpscalingMode::Off;
        if (name == "FSR3.1") return UpscalingMode::Fsr31;
        throw std::invalid_argument("Unknown upscaling mode");
    }

    [[nodiscard]] inline UpscalingQuality ParseUpscalingQuality(std::string_view name)
    {
        for (size_t index = 0; index < UpscalingQualityNames.size(); ++index)
            if (name == UpscalingQualityNames[index]) return static_cast<UpscalingQuality>(index);
        throw std::invalid_argument("Unknown upscaling quality");
    }

    // FSR quality is relative to output resolution, not the legacy render-height
    // cap. Off/fallback callers keep using ResolveRenderExtent instead.
    [[nodiscard]] inline RenderExtent ResolveUpscalingExtent(uint32_t outputWidth,
        uint32_t outputHeight, UpscalingQuality quality)
    {
        if (outputWidth == 0 || outputHeight == 0) return {0, 0};
        constexpr std::array ratios{1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
        const float ratio = ratios.at(static_cast<size_t>(quality));
        return {std::max(1u, static_cast<uint32_t>(float(outputWidth) / ratio)),
            std::max(1u, static_cast<uint32_t>(float(outputHeight) / ratio))};
    }
}
