#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <limits>

namespace HEngine
{
    inline constexpr float c_GlobalIlluminationDiffuseAlbedoEpsilon = 1e-4f;
    inline constexpr uint32_t c_InvalidGlobalIlluminationMaterialId =
        std::numeric_limits<uint32_t>::max();

    struct GlobalIlluminationDiffuseSignal
    {
        glm::vec3 DiffuseAlbedo{ 0.0f };
        glm::vec3 DemodulatedIllumination{ 0.0f };
        bool bHasDiffuseResponse = false;
    };

    [[nodiscard]] glm::vec3 ComputeGlobalIlluminationDiffuseAlbedo(
        const glm::vec3& baseColor,
        float metallic) noexcept;

    // Emissive and fully-metallic surfaces do not own a diffuse GI signal.
    // Zero-albedo components remain zero instead of dividing by epsilon.
    [[nodiscard]] GlobalIlluminationDiffuseSignal
        DemodulateGlobalIlluminationDiffuse(
            const glm::vec3& diffuseIndirectRadiance,
            const glm::vec3& baseColor,
            float metallic,
            bool bEmissive,
            float albedoEpsilon =
                c_GlobalIlluminationDiffuseAlbedoEpsilon) noexcept;

    [[nodiscard]] glm::vec3 RemodulateGlobalIlluminationDiffuse(
        const GlobalIlluminationDiffuseSignal& signal) noexcept;

    // Maps the center of a trace-resolution pixel to a clamped full-resolution
    // pixel. Invalid dimensions map to the origin.
    [[nodiscard]] glm::uvec2 MapGlobalIlluminationTracePixelToFullPixel(
        const glm::uvec2& tracePixel,
        const glm::uvec2& traceDimensions,
        const glm::uvec2& fullDimensions) noexcept;

    struct GlobalIlluminationSpatialSurface
    {
        uint32_t MaterialId = c_InvalidGlobalIlluminationMaterialId;
        glm::vec3 DiffuseAlbedo{ 0.0f };
        float Roughness = 1.0f;
    };

    // Material boundaries are hard barriers. Within one material, albedo and
    // roughness differences smoothly reduce compatibility in [0, 1].
    [[nodiscard]] float ComputeGlobalIlluminationSpatialCompatibility(
        const GlobalIlluminationSpatialSurface& center,
        const GlobalIlluminationSpatialSurface& sample,
        float albedoSigma = 0.25f,
        float roughnessSigma = 0.25f) noexcept;
}
