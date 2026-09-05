#pragma once

#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Graphics/RHI/RenderBackend.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace HEngine
{
    enum class AntiAliasingMode : uint8_t
    {
        None,
        Fxaa,
        Smaa,
        Taa,
        Msaa,
    };

    enum class AntiAliasingRenderPath : uint8_t
    {
        RealtimeRaster,
        OfflinePathTracing,
    };

    struct AntiAliasingSettings
    {
        AntiAliasingMode Mode = AntiAliasingMode::None;
        uint32_t MsaaSamples = 4;
        float FxaaEdgeThreshold = 0.125f;
        float FxaaEdgeThresholdMin = 0.0312f;
        float FxaaSubpixelQuality = 0.75f;
        float SmaaEdgeThreshold = 0.06f;
        uint32_t SmaaMaxSearchSteps = 24;
        uint32_t SmaaMaxDiagonalSearchSteps = 8;
        float SmaaCornerRounding = 0.25f;
        float TaaHistoryWeight = 0.92f;
        float TaaSharpness = 0.15f;
        uint32_t CaptureSupersampleScale = 1;

        [[nodiscard]] bool operator==(const AntiAliasingSettings&) const = default;
    };

    struct AntiAliasingAvailability
    {
        bool bAvailable = false;
        std::string Reason;
    };

    struct AntiAliasingCapabilities
    {
        AntiAliasingAvailability Fxaa;
        AntiAliasingAvailability Smaa;
        AntiAliasingAvailability Taa;
        AntiAliasingAvailability Msaa2x;
        AntiAliasingAvailability Msaa4x;
        AntiAliasingAvailability Msaa8x;
    };

    struct AntiAliasingSelection
    {
        AntiAliasingSettings Requested;
        AntiAliasingSettings Effective;
        bool bRequestedModeAvailable = true;
        bool bFallback = false;
        bool bBypassed = false;
        std::string Reason;
    };

    [[nodiscard]] const char* ToString(AntiAliasingMode mode) noexcept;
    [[nodiscard]] AntiAliasingMode ParseAntiAliasingMode(std::string_view name);

    [[nodiscard]] bool IsValidMsaaSampleCount(uint32_t sampleCount) noexcept;
    [[nodiscard]] bool IsValidCaptureSupersampleScale(uint32_t scale) noexcept;
    [[nodiscard]] bool IsValidTaaHistoryWeight(float historyWeight) noexcept;
    [[nodiscard]] bool IsValidTaaSharpness(float sharpness) noexcept;
    [[nodiscard]] bool IsValidAntiAliasingSettings(
        const AntiAliasingSettings& settings) noexcept;
    void ValidateAntiAliasingSettings(const AntiAliasingSettings& settings);

    [[nodiscard]] uint32_t GetAntiAliasingSampleCount(
        const AntiAliasingSettings& settings);
    [[nodiscard]] AntiAliasingSelection ResolveAntiAliasingSelection(
        const AntiAliasingSettings& requested,
        const AntiAliasingCapabilities& capabilities,
        AntiAliasingRenderPath renderPath =
            AntiAliasingRenderPath::RealtimeRaster);
    [[nodiscard]] AntiAliasingCapabilities BuildAntiAliasingCapabilities(
        RenderBackend backend,
        const RenderCapabilities& capabilities);

    [[nodiscard]] Json SerializeAntiAliasingSettings(
        const AntiAliasingSettings& settings);
    [[nodiscard]] AntiAliasingSettings DeserializeAntiAliasingSettings(
        const Json& json,
        const AntiAliasingSettings& defaults = {});
}
