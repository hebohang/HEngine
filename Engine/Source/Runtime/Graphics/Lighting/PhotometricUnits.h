#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace HEngine::Photometry
{
    enum class LightUnit : int { Unitless = 0, Candela = 1, Lumen = 2 };

    inline float NonNegative(float value) noexcept
    {
        return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
    }

    inline float SolidAngle(float outerHalfAngleDegrees = 180.0f) noexcept
    {
        const double angle = std::isfinite(outerHalfAngleDegrees)
            ? std::clamp(double(outerHalfAngleDegrees), 0.1, 180.0) : 180.0;
        return float(2.0 * std::numbers::pi *
            (1.0 - std::cos(angle * std::numbers::pi / 180.0)));
    }

    inline double CandelaScale(int unit, float solidAngle) noexcept
    {
        if (unit == int(LightUnit::Lumen))
            return 1.0 / std::max(double(NonNegative(solidAngle)), 1e-6);
        // Conventional dimensionless local-light scale; evaluated only on CPU.
        if (unit == int(LightUnit::Unitless))
            return 1.0 / 625.0;
        return unit == int(LightUnit::Candela) ? 1.0 : 0.0;
    }

    inline float ConvertIntensity(float intensity, int from, int to,
        float solidAngle = SolidAngle()) noexcept
    {
        const double destinationScale = CandelaScale(to, solidAngle);
        if (destinationScale <= 0.0) return 0.0f;
        const double converted = double(NonNegative(intensity)) *
            CandelaScale(from, solidAngle) / destinationScale;
        return float(std::min(converted, double(std::numeric_limits<float>::max())));
    }

    inline float ToCandela(float intensity, int unit,
        float outerHalfAngleDegrees = 180.0f) noexcept
    {
        return ConvertIntensity(intensity, unit, int(LightUnit::Candela),
            SolidAngle(outerHalfAngleDegrees));
    }

    // Fixed camera exposure with neutral lens calibration. Positive compensation
    // brightens by powers of two; positive EV100 requires less exposure.
    inline float ExposureMultiplier(float ev100, float compensation = 0.0f) noexcept
    {
        const float ev = std::isfinite(ev100) ? std::clamp(ev100, -24.0f, 32.0f) : 0.0f;
        const float bias = std::isfinite(compensation)
            ? std::clamp(compensation, -24.0f, 24.0f) : 0.0f;
        return std::exp2(std::clamp(bias - ev, -32.0f, 24.0f));
    }
}
