#include "Hepch.h"

#include "Runtime/Graphics/Renderer/AntiAliasing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace HEngine
{
    namespace
    {
        constexpr float c_MaxTaaHistoryWeight = 0.999f;
        constexpr float c_MaxTaaSharpness = 1.0f;
        constexpr uint32_t c_MaxSmaaSearchSteps = 32;
        constexpr uint32_t c_MaxSmaaDiagonalSearchSteps = 16;
        constexpr std::array<std::string_view, 14> c_SettingFields = {
            "Mode",
            "MsaaSamples",
            "FxaaEdgeThreshold",
            "FxaaEdgeThresholdMin",
            "FxaaSubpixelQuality",
            "SmaaEdgeThreshold",
            "SmaaMaxSearchSteps",
            "SmaaMaxDiagonalSearchSteps",
            "SmaaCornerRounding",
            "TaaHistoryWeight",
            "TaaSharpness",
            "TaaOptimizedHistoryGather",
            "TaaSharedNeighborhood",
            "CaptureSupersampleScale",
        };

        [[nodiscard]] bool IsFiniteInRange(
            float value,
            float minimum,
            float maximum) noexcept
        {
            return std::isfinite(value) && value >= minimum && value <= maximum;
        }

        [[nodiscard]] bool IsKnownMode(AntiAliasingMode mode) noexcept
        {
            switch (mode)
            {
            case AntiAliasingMode::None:
            case AntiAliasingMode::Fxaa:
            case AntiAliasingMode::Smaa:
            case AntiAliasingMode::Taa:
            case AntiAliasingMode::Msaa:
                return true;
            }
            return false;
        }

        [[nodiscard]] bool IsKnownSettingField(std::string_view name) noexcept
        {
            return std::ranges::find(c_SettingFields, name) != c_SettingFields.end();
        }

        [[nodiscard]] uint32_t ReadUnsignedField(
            const Json& value,
            std::string_view fieldName)
        {
            if (!value.is_number_integer())
            {
                throw std::invalid_argument(
                    "Anti-aliasing field '" + std::string(fieldName) +
                    "' must be an unsigned integer");
            }

            uint64_t unsignedValue = 0;
            if (value.is_number_unsigned())
            {
                unsignedValue = value.get<uint64_t>();
            }
            else
            {
                const int64_t signedValue = value.get<int64_t>();
                if (signedValue < 0)
                {
                    throw std::invalid_argument(
                        "Anti-aliasing field '" + std::string(fieldName) +
                        "' cannot be negative");
                }
                unsignedValue = static_cast<uint64_t>(signedValue);
            }

            if (unsignedValue > std::numeric_limits<uint32_t>::max())
            {
                throw std::invalid_argument(
                    "Anti-aliasing field '" + std::string(fieldName) +
                    "' exceeds uint32 range");
            }
            return static_cast<uint32_t>(unsignedValue);
        }

        [[nodiscard]] float ReadFloatField(
            const Json& value,
            std::string_view fieldName)
        {
            if (!value.is_number())
            {
                throw std::invalid_argument(
                    "Anti-aliasing field '" + std::string(fieldName) +
                    "' must be numeric");
            }
            const double result = value.get<double>();
            if (!std::isfinite(result) ||
                result < -std::numeric_limits<float>::max() ||
                result > std::numeric_limits<float>::max())
            {
                throw std::invalid_argument(
                    "Anti-aliasing field '" + std::string(fieldName) +
                    "' must be a finite float");
            }
            return static_cast<float>(result);
        }

        [[nodiscard]] AntiAliasingAvailability GetRequestedAvailability(
            const AntiAliasingSettings& requested,
            const AntiAliasingCapabilities& capabilities)
        {
            switch (requested.Mode)
            {
            case AntiAliasingMode::None:
                return { true, "Anti-aliasing is disabled." };
            case AntiAliasingMode::Fxaa:
                return capabilities.Fxaa;
            case AntiAliasingMode::Smaa:
                return capabilities.Smaa;
            case AntiAliasingMode::Taa:
                return capabilities.Taa;
            case AntiAliasingMode::Msaa:
                switch (requested.MsaaSamples)
                {
                case 2: return capabilities.Msaa2x;
                case 4: return capabilities.Msaa4x;
                case 8: return capabilities.Msaa8x;
                }
                break;
            }
            return { false, "The requested anti-aliasing mode is invalid." };
        }

        [[nodiscard]] std::string DefaultUnavailableReason(
            const AntiAliasingSettings& requested)
        {
            if (requested.Mode == AntiAliasingMode::Msaa)
            {
                return "MSAA " + std::to_string(requested.MsaaSamples) +
                    "x is unavailable.";
            }
            return std::string(ToString(requested.Mode)) + " is unavailable.";
        }
    }

    const char* ToString(AntiAliasingMode mode) noexcept
    {
        switch (mode)
        {
        case AntiAliasingMode::None: return "None";
        case AntiAliasingMode::Fxaa: return "FXAA";
        case AntiAliasingMode::Smaa: return "SMAA";
        case AntiAliasingMode::Taa: return "TAA";
        case AntiAliasingMode::Msaa: return "MSAA";
        }
        return "Unknown";
    }

    AntiAliasingMode ParseAntiAliasingMode(std::string_view name)
    {
        if (name == "None")
            return AntiAliasingMode::None;
        if (name == "FXAA")
            return AntiAliasingMode::Fxaa;
        if (name == "SMAA")
            return AntiAliasingMode::Smaa;
        if (name == "TAA")
            return AntiAliasingMode::Taa;
        if (name == "MSAA")
            return AntiAliasingMode::Msaa;
        throw std::invalid_argument(
            "Unknown anti-aliasing mode '" + std::string(name) + "'");
    }

    bool IsValidMsaaSampleCount(uint32_t sampleCount) noexcept
    {
        return sampleCount == 2 || sampleCount == 4 || sampleCount == 8;
    }

    bool IsValidCaptureSupersampleScale(uint32_t scale) noexcept
    {
        return scale == 1 || scale == 2 || scale == 4;
    }

    bool IsValidTaaHistoryWeight(float historyWeight) noexcept
    {
        return std::isfinite(historyWeight) && historyWeight >= 0.0f &&
            historyWeight <= c_MaxTaaHistoryWeight;
    }

    bool IsValidTaaSharpness(float sharpness) noexcept
    {
        return std::isfinite(sharpness) && sharpness >= 0.0f &&
            sharpness <= c_MaxTaaSharpness;
    }

    bool IsValidAntiAliasingSettings(
        const AntiAliasingSettings& settings) noexcept
    {
        return IsKnownMode(settings.Mode) &&
            IsValidMsaaSampleCount(settings.MsaaSamples) &&
            IsFiniteInRange(settings.FxaaEdgeThreshold, 1e-4f, 1.0f) &&
            IsFiniteInRange(settings.FxaaEdgeThresholdMin, 0.0f, 1.0f) &&
            IsFiniteInRange(settings.FxaaSubpixelQuality, 0.0f, 1.0f) &&
            IsFiniteInRange(settings.SmaaEdgeThreshold, 1e-4f, 1.0f) &&
            settings.SmaaMaxSearchSteps >= 1 &&
            settings.SmaaMaxSearchSteps <= c_MaxSmaaSearchSteps &&
            settings.SmaaMaxDiagonalSearchSteps <=
                c_MaxSmaaDiagonalSearchSteps &&
            IsFiniteInRange(settings.SmaaCornerRounding, 0.0f, 1.0f) &&
            IsValidTaaHistoryWeight(settings.TaaHistoryWeight) &&
            IsValidTaaSharpness(settings.TaaSharpness) &&
            IsValidCaptureSupersampleScale(
                settings.CaptureSupersampleScale);
    }

    void ValidateAntiAliasingSettings(const AntiAliasingSettings& settings)
    {
        if (!IsKnownMode(settings.Mode))
            throw std::invalid_argument("Anti-aliasing mode is invalid");
        if (!IsValidMsaaSampleCount(settings.MsaaSamples))
        {
            throw std::invalid_argument(
                "Anti-aliasing MsaaSamples must be 2, 4, or 8");
        }
        if (!IsFiniteInRange(settings.FxaaEdgeThreshold, 1e-4f, 1.0f))
        {
            throw std::invalid_argument(
                "Anti-aliasing FxaaEdgeThreshold must be finite and in [0.0001, 1]");
        }
        if (!IsFiniteInRange(settings.FxaaEdgeThresholdMin, 0.0f, 1.0f))
        {
            throw std::invalid_argument(
                "Anti-aliasing FxaaEdgeThresholdMin must be finite and in [0, 1]");
        }
        if (!IsFiniteInRange(settings.FxaaSubpixelQuality, 0.0f, 1.0f))
        {
            throw std::invalid_argument(
                "Anti-aliasing FxaaSubpixelQuality must be finite and in [0, 1]");
        }
        if (!IsFiniteInRange(settings.SmaaEdgeThreshold, 1e-4f, 1.0f))
        {
            throw std::invalid_argument(
                "Anti-aliasing SmaaEdgeThreshold must be finite and in [0.0001, 1]");
        }
        if (settings.SmaaMaxSearchSteps < 1 ||
            settings.SmaaMaxSearchSteps > c_MaxSmaaSearchSteps)
        {
            throw std::invalid_argument(
                "Anti-aliasing SmaaMaxSearchSteps must be in [1, 32]");
        }
        if (settings.SmaaMaxDiagonalSearchSteps >
            c_MaxSmaaDiagonalSearchSteps)
        {
            throw std::invalid_argument(
                "Anti-aliasing SmaaMaxDiagonalSearchSteps must be in [0, 16]");
        }
        if (!IsFiniteInRange(settings.SmaaCornerRounding, 0.0f, 1.0f))
        {
            throw std::invalid_argument(
                "Anti-aliasing SmaaCornerRounding must be finite and in [0, 1]");
        }
        if (!IsValidTaaHistoryWeight(settings.TaaHistoryWeight))
        {
            throw std::invalid_argument(
                "Anti-aliasing TaaHistoryWeight must be finite and in [0, 0.999]");
        }
        if (!IsValidTaaSharpness(settings.TaaSharpness))
        {
            throw std::invalid_argument(
                "Anti-aliasing TaaSharpness must be finite and in [0, 1]");
        }
        if (!IsValidCaptureSupersampleScale(
                settings.CaptureSupersampleScale))
        {
            throw std::invalid_argument(
                "Anti-aliasing CaptureSupersampleScale must be 1, 2, or 4");
        }
    }

    uint32_t GetAntiAliasingSampleCount(
        const AntiAliasingSettings& settings)
    {
        if (!IsKnownMode(settings.Mode))
            throw std::invalid_argument("Anti-aliasing mode is invalid");
        if (settings.Mode != AntiAliasingMode::Msaa)
            return 1;
        if (!IsValidMsaaSampleCount(settings.MsaaSamples))
        {
            throw std::invalid_argument(
                "Anti-aliasing MsaaSamples must be 2, 4, or 8");
        }
        return settings.MsaaSamples;
    }

    AntiAliasingSelection ResolveAntiAliasingSelection(
        const AntiAliasingSettings& requested,
        const AntiAliasingCapabilities& capabilities,
        AntiAliasingRenderPath renderPath)
    {
        ValidateAntiAliasingSettings(requested);
        AntiAliasingSelection selection{
            .Requested = requested,
            .Effective = requested,
        };

        if (renderPath == AntiAliasingRenderPath::OfflinePathTracing)
        {
            selection.Effective.Mode = AntiAliasingMode::None;
            selection.Effective.CaptureSupersampleScale = 1;
            selection.bBypassed = true;
            selection.Reason =
                "Offline path tracing provides stochastic subpixel sampling; "
                "realtime anti-aliasing is bypassed.";
            return selection;
        }

        const AntiAliasingAvailability availability =
            GetRequestedAvailability(requested, capabilities);
        selection.bRequestedModeAvailable = availability.bAvailable;
        if (availability.bAvailable)
        {
            selection.Reason = availability.Reason.empty()
                ? "The requested anti-aliasing mode is available."
                : availability.Reason;
            return selection;
        }

        selection.Effective.Mode = AntiAliasingMode::None;
        selection.bFallback = requested.Mode != AntiAliasingMode::None;
        selection.Reason = availability.Reason.empty()
            ? DefaultUnavailableReason(requested)
            : availability.Reason;
        return selection;
    }

    AntiAliasingCapabilities BuildAntiAliasingCapabilities(
        RenderBackend backend,
        const RenderCapabilities& capabilities)
    {
        const bool bGraphics = capabilities.bGraphicsQueue;
        const bool bCompute = capabilities.bComputeQueue;
        AntiAliasingCapabilities result;
        result.Fxaa = {
            bGraphics,
            bGraphics
                ? std::string("FXAA is available on the active ") +
                    ToString(backend) + " graphics queue."
                : "FXAA requires a graphics queue.",
        };
        result.Smaa = {
            bGraphics,
            bGraphics
                ? std::string("SMAA 1x is available on the active ") +
                    ToString(backend) + " graphics queue."
                : "SMAA 1x requires a graphics queue.",
        };
        result.Taa = {
            bGraphics && bCompute,
            bGraphics && bCompute
                ? "TAA is available with motion/depth history support."
                : "TAA requires graphics and compute queues.",
        };
        const auto msaaAvailability =
            [bGraphics, &capabilities](uint32_t sampleCount)
            {
                const bool bAvailable = bGraphics &&
                    sampleCount < 32u &&
                    (capabilities.SupportedMsaaSampleMask &
                        (1u << sampleCount)) != 0u;
                return AntiAliasingAvailability{
                    bAvailable,
                    bAvailable
                        ? "The active adapter supports the requested scene color/depth sample count."
                        : "The active adapter does not support the requested RGBA16_FLOAT + D32 MSAA sample count.",
                };
            };
        result.Msaa2x = msaaAvailability(2u);
        result.Msaa4x = msaaAvailability(4u);
        result.Msaa8x = msaaAvailability(8u);
        return result;
    }

    Json SerializeAntiAliasingSettings(
        const AntiAliasingSettings& settings)
    {
        AntiAliasingSettings persistentSettings = settings;
        persistentSettings.CaptureSupersampleScale = 1;
        ValidateAntiAliasingSettings(persistentSettings);
        return Json{
            { "Mode", ToString(persistentSettings.Mode) },
            { "MsaaSamples", persistentSettings.MsaaSamples },
            { "FxaaEdgeThreshold", persistentSettings.FxaaEdgeThreshold },
            { "FxaaEdgeThresholdMin", persistentSettings.FxaaEdgeThresholdMin },
            { "FxaaSubpixelQuality", persistentSettings.FxaaSubpixelQuality },
            { "SmaaEdgeThreshold", persistentSettings.SmaaEdgeThreshold },
            { "SmaaMaxSearchSteps", persistentSettings.SmaaMaxSearchSteps },
            { "SmaaMaxDiagonalSearchSteps",
                persistentSettings.SmaaMaxDiagonalSearchSteps },
            { "SmaaCornerRounding", persistentSettings.SmaaCornerRounding },
            { "TaaHistoryWeight", persistentSettings.TaaHistoryWeight },
            { "TaaSharpness", persistentSettings.TaaSharpness },
            { "TaaOptimizedHistoryGather", persistentSettings.bTaaOptimizedHistoryGather },
            { "TaaSharedNeighborhood", persistentSettings.bTaaSharedNeighborhood },
        };
    }

    AntiAliasingSettings DeserializeAntiAliasingSettings(
        const Json& json,
        const AntiAliasingSettings& defaults)
    {
        ValidateAntiAliasingSettings(defaults);
        if (!json.is_object())
        {
            throw std::invalid_argument(
                "Anti-aliasing settings must be a JSON object");
        }
        for (auto iterator = json.begin(); iterator != json.end(); ++iterator)
        {
            if (!IsKnownSettingField(iterator.key()))
            {
                throw std::invalid_argument(
                    "Unknown anti-aliasing setting field '" + iterator.key() +
                    "'");
            }
        }

        AntiAliasingSettings result = defaults;
        if (const auto iterator = json.find("Mode"); iterator != json.end())
        {
            if (!iterator->is_string())
            {
                throw std::invalid_argument(
                    "Anti-aliasing field 'Mode' must be a string");
            }
            result.Mode = ParseAntiAliasingMode(
                iterator->get_ref<const std::string&>());
        }
        if (const auto iterator = json.find("MsaaSamples"); iterator != json.end())
        {
            result.MsaaSamples = ReadUnsignedField(*iterator, "MsaaSamples");
        }
        if (const auto iterator = json.find("FxaaEdgeThreshold");
            iterator != json.end())
        {
            result.FxaaEdgeThreshold = ReadFloatField(
                *iterator,
                "FxaaEdgeThreshold");
        }
        if (const auto iterator = json.find("FxaaEdgeThresholdMin");
            iterator != json.end())
        {
            result.FxaaEdgeThresholdMin = ReadFloatField(
                *iterator,
                "FxaaEdgeThresholdMin");
        }
        if (const auto iterator = json.find("FxaaSubpixelQuality");
            iterator != json.end())
        {
            result.FxaaSubpixelQuality = ReadFloatField(
                *iterator,
                "FxaaSubpixelQuality");
        }
        if (const auto iterator = json.find("SmaaEdgeThreshold");
            iterator != json.end())
        {
            result.SmaaEdgeThreshold = ReadFloatField(
                *iterator,
                "SmaaEdgeThreshold");
        }
        if (const auto iterator = json.find("SmaaMaxSearchSteps");
            iterator != json.end())
        {
            result.SmaaMaxSearchSteps = ReadUnsignedField(
                *iterator,
                "SmaaMaxSearchSteps");
        }
        if (const auto iterator = json.find("SmaaMaxDiagonalSearchSteps");
            iterator != json.end())
        {
            result.SmaaMaxDiagonalSearchSteps = ReadUnsignedField(
                *iterator,
                "SmaaMaxDiagonalSearchSteps");
        }
        if (const auto iterator = json.find("SmaaCornerRounding");
            iterator != json.end())
        {
            result.SmaaCornerRounding = ReadFloatField(
                *iterator,
                "SmaaCornerRounding");
        }
        if (const auto iterator = json.find("TaaHistoryWeight");
            iterator != json.end())
        {
            result.TaaHistoryWeight = ReadFloatField(
                *iterator,
                "TaaHistoryWeight");
        }
        if (const auto iterator = json.find("TaaSharpness");
            iterator != json.end())
        {
            result.TaaSharpness = ReadFloatField(*iterator, "TaaSharpness");
        }
        if (const auto iterator = json.find("TaaOptimizedHistoryGather"); iterator != json.end())
        {
            if (!iterator->is_boolean())
                throw std::invalid_argument("TaaOptimizedHistoryGather must be a boolean");
            result.bTaaOptimizedHistoryGather = iterator->get<bool>();
        }
        if (const auto iterator = json.find("TaaSharedNeighborhood"); iterator != json.end())
        {
            if (!iterator->is_boolean())
                throw std::invalid_argument("TaaSharedNeighborhood must be a boolean");
            result.bTaaSharedNeighborhood = iterator->get<bool>();
        }
        if (const auto iterator = json.find("CaptureSupersampleScale");
            iterator != json.end())
        {
            result.CaptureSupersampleScale = ReadUnsignedField(
                *iterator,
                "CaptureSupersampleScale");
        }
        ValidateAntiAliasingSettings(result);
        return result;
    }
}
