#include "Hepch.h"

#include "Runtime/Graphics/GlobalIllumination/GlobalIlluminationSignal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] float SanitizeNonNegative(float value) noexcept
        {
            return std::isfinite(value) && value > 0.0f ? value : 0.0f;
        }

        [[nodiscard]] glm::vec3 SanitizeNonNegative(
            const glm::vec3& value) noexcept
        {
            return {
                SanitizeNonNegative(value.x),
                SanitizeNonNegative(value.y),
                SanitizeNonNegative(value.z),
            };
        }

        [[nodiscard]] uint32_t MapPixelAxis(
            uint32_t traceCoordinate,
            uint32_t traceDimension,
            uint32_t fullDimension) noexcept
        {
            if (traceDimension == 0 || fullDimension == 0)
                return 0;

            const uint64_t clampedCoordinate = std::min(
                traceCoordinate,
                traceDimension - 1u);
            const uint64_t numerator =
                (clampedCoordinate * 2u + 1u) * fullDimension;
            const uint64_t denominator =
                static_cast<uint64_t>(traceDimension) * 2u;
            return static_cast<uint32_t>(std::min<uint64_t>(
                numerator / denominator,
                fullDimension - 1u));
        }

        [[nodiscard]] bool IsFinite(const glm::vec3& value) noexcept
        {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                std::isfinite(value.z);
        }
    }

    glm::vec3 ComputeGlobalIlluminationDiffuseAlbedo(
        const glm::vec3& baseColor,
        float metallic) noexcept
    {
        const float finiteMetallic = std::isfinite(metallic) ? metallic : 1.0f;
        const float diffuseFraction =
            1.0f - std::clamp(finiteMetallic, 0.0f, 1.0f);
        return SanitizeNonNegative(baseColor) * diffuseFraction;
    }

    GlobalIlluminationDiffuseSignal DemodulateGlobalIlluminationDiffuse(
        const glm::vec3& diffuseIndirectRadiance,
        const glm::vec3& baseColor,
        float metallic,
        bool bEmissive,
        float albedoEpsilon) noexcept
    {
        GlobalIlluminationDiffuseSignal signal;
        signal.DiffuseAlbedo = ComputeGlobalIlluminationDiffuseAlbedo(
            baseColor,
            metallic);

        const float finiteMetallic = std::isfinite(metallic) ? metallic : 1.0f;
        if (bEmissive || finiteMetallic >= 1.0f)
        {
            signal.DiffuseAlbedo = glm::vec3(0.0f);
            return signal;
        }

        const float epsilon = std::isfinite(albedoEpsilon) &&
                albedoEpsilon > 0.0f
            ? albedoEpsilon
            : c_GlobalIlluminationDiffuseAlbedoEpsilon;
        const glm::vec3 radiance = SanitizeNonNegative(diffuseIndirectRadiance);
        for (uint32_t component = 0; component < 3; ++component)
        {
            const float albedo = signal.DiffuseAlbedo[component];
            signal.DemodulatedIllumination[component] = albedo > 0.0f
                ? radiance[component] / std::max(albedo, epsilon)
                : 0.0f;
        }
        signal.bHasDiffuseResponse =
            signal.DiffuseAlbedo.x > 0.0f ||
            signal.DiffuseAlbedo.y > 0.0f ||
            signal.DiffuseAlbedo.z > 0.0f;
        return signal;
    }

    glm::vec3 RemodulateGlobalIlluminationDiffuse(
        const GlobalIlluminationDiffuseSignal& signal) noexcept
    {
        if (!signal.bHasDiffuseResponse ||
            !IsFinite(signal.DiffuseAlbedo) ||
            !IsFinite(signal.DemodulatedIllumination))
        {
            return glm::vec3(0.0f);
        }
        return signal.DemodulatedIllumination * signal.DiffuseAlbedo;
    }

    glm::uvec2 MapGlobalIlluminationTracePixelToFullPixel(
        const glm::uvec2& tracePixel,
        const glm::uvec2& traceDimensions,
        const glm::uvec2& fullDimensions) noexcept
    {
        return {
            MapPixelAxis(tracePixel.x, traceDimensions.x, fullDimensions.x),
            MapPixelAxis(tracePixel.y, traceDimensions.y, fullDimensions.y),
        };
    }

    float ComputeGlobalIlluminationSpatialCompatibility(
        const GlobalIlluminationSpatialSurface& center,
        const GlobalIlluminationSpatialSurface& sample,
        float albedoSigma,
        float roughnessSigma) noexcept
    {
        if (center.MaterialId == c_InvalidGlobalIlluminationMaterialId ||
            sample.MaterialId == c_InvalidGlobalIlluminationMaterialId ||
            center.MaterialId != sample.MaterialId ||
            !IsFinite(center.DiffuseAlbedo) ||
            !IsFinite(sample.DiffuseAlbedo) ||
            !std::isfinite(center.Roughness) ||
            !std::isfinite(sample.Roughness))
        {
            return 0.0f;
        }

        const float safeAlbedoSigma = std::isfinite(albedoSigma)
            ? std::max(std::abs(albedoSigma), 1e-6f)
            : 1e-6f;
        const float safeRoughnessSigma = std::isfinite(roughnessSigma)
            ? std::max(std::abs(roughnessSigma), 1e-6f)
            : 1e-6f;
        const glm::vec3 albedoDifference =
            center.DiffuseAlbedo - sample.DiffuseAlbedo;
        const float roughnessDifference =
            center.Roughness - sample.Roughness;
        const float exponent = std::min(
            glm::dot(albedoDifference, albedoDifference) /
                    (2.0f * safeAlbedoSigma * safeAlbedoSigma) +
                roughnessDifference * roughnessDifference /
                    (2.0f * safeRoughnessSigma * safeRoughnessSigma),
            80.0f);
        return std::clamp(std::exp(-exponent), 0.0f, 1.0f);
    }
}
