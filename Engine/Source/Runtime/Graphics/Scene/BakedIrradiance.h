#pragma once

#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

namespace HEngine
{
    inline constexpr size_t MaximumBakedIrradianceVolumes = 16;

    inline bool IsValidBakedIrradiance(const RenderBakedIrradianceVolume& volume)
    {
        const auto finite = [](const glm::vec3& value)
        { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); };
        if (!finite(volume.BoundsMin) || !finite(volume.BoundsMax) ||
            !glm::all(glm::greaterThan(volume.BoundsMax, volume.BoundsMin)) ||
            !std::isfinite(volume.Intensity) || volume.Intensity <= 0.0f || volume.Intensity > 1000.0f ||
            !std::isfinite(volume.BlendDistance) || volume.BlendDistance <= 0.0f)
            return false;
        return std::all_of(volume.IrradianceSH.begin(), volume.IrradianceSH.end(), [&](const auto& row)
        { return finite(row) && glm::all(glm::lessThanEqual(glm::abs(row), glm::vec3(1e8f))); });
    }

    // Packed identically in BakedIrradiance.hlsli; appended to the existing frame
    // prefix so depth/sky shaders need not consume the new fields.
    struct alignas(16) BakedIrradianceGpuVolume
    {
        glm::vec4 BoundsMinBlend{0};
        glm::vec4 BoundsMaxIntensity{0};
        std::array<glm::vec4, 9> SH{};
    };
    struct alignas(16) BakedIrradianceGpuData
    {
        glm::uvec4 Count{0};
        std::array<BakedIrradianceGpuVolume, MaximumBakedIrradianceVolumes> Volumes{};
    };
    static_assert(sizeof(BakedIrradianceGpuVolume) == 176);
    static_assert(sizeof(BakedIrradianceGpuData) == 2832);

    inline BakedIrradianceGpuData PrepareBakedIrradiance(
        std::span<const RenderBakedIrradianceVolume> volumes, const glm::vec3& camera)
    {
        std::vector<const RenderBakedIrradianceVolume*> valid;
        for (const auto& volume : volumes)
            if (IsValidBakedIrradiance(volume)) valid.push_back(&volume);
        const auto distance = [&](const auto* volume)
        {
            const auto delta = camera - glm::clamp(camera, volume->BoundsMin, volume->BoundsMax);
            return glm::dot(delta, delta);
        };
        std::stable_sort(valid.begin(), valid.end(), [&](auto* a, auto* b)
        {
            const float da = distance(a), db = distance(b);
            return da == db ? a->EntityId < b->EntityId : da < db;
        });
        BakedIrradianceGpuData result;
        result.Count.x = static_cast<uint32_t>(std::min(valid.size(), MaximumBakedIrradianceVolumes));
        for (uint32_t i = 0; i < result.Count.x; ++i)
        {
            const auto& source = *valid[i];
            auto& target = result.Volumes[i];
            target.BoundsMinBlend = glm::vec4(source.BoundsMin, std::max(source.BlendDistance, 0.001f));
            target.BoundsMaxIntensity = glm::vec4(source.BoundsMax, source.Intensity);
            for (size_t j = 0; j < 9; ++j) target.SH[j] = glm::vec4(source.IrradianceSH[j], 0);
        }
        return result;
    }
}
