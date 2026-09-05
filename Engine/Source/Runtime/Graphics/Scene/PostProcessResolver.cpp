#include "Hepch.h"

#include "Runtime/Graphics/Scene/PostProcessResolver.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] float SanitizeExposure(float value, float fallback) noexcept
        {
            return std::isfinite(value) ? std::max(value, 0.0f) : fallback;
        }

        [[nodiscard]] float SanitizePriority(float value) noexcept
        {
            return std::isfinite(value) ? value : 0.0f;
        }

        [[nodiscard]] float DistanceToVolumeBox(
            const glm::mat4& transform,
            const glm::vec3& position) noexcept
        {
            const glm::vec3 center(transform[3]);
            const glm::vec3 centerToPosition = position - center;
            float outsideDistanceSquared = 0.0f;
            constexpr std::array<glm::vec3, 3> fallbackAxes = {
                glm::vec3(1.0f, 0.0f, 0.0f),
                glm::vec3(0.0f, 1.0f, 0.0f),
                glm::vec3(0.0f, 0.0f, 1.0f),
            };

            for (std::size_t axisIndex = 0; axisIndex < 3; ++axisIndex)
            {
                const glm::vec3 axisVector(transform[axisIndex]);
                const float axisLength = glm::length(axisVector);
                const bool bUsableAxis = std::isfinite(axisLength) &&
                    axisLength > 1e-6f;
                const glm::vec3 axis = bUsableAxis
                    ? axisVector / axisLength
                    : fallbackAxes[axisIndex];
                const float halfExtent = bUsableAxis ? axisLength * 0.5f : 0.0f;
                const float outside = std::max(
                    std::abs(glm::dot(centerToPosition, axis)) - halfExtent,
                    0.0f);
                outsideDistanceSquared += outside * outside;
            }
            return std::sqrt(outsideDistanceSquared);
        }
    }

    float ComputePostProcessVolumeInfluence(
        const PostProcessVolumeSnapshot& volume,
        const glm::vec3& cameraPosition) noexcept
    {
        if (!volume.bEnabled)
            return 0.0f;
        if (volume.bGlobal)
            return 1.0f;
        if (!volume.bLocalVolumeEnabled)
            return 0.0f;

        const float distance = DistanceToVolumeBox(
            volume.WorldTransform,
            cameraPosition);
        if (!std::isfinite(distance))
            return 0.0f;
        if (distance <= 1e-6f)
            return 1.0f;

        const float blendRadius = std::isfinite(volume.BlendRadius)
            ? std::max(volume.BlendRadius, 0.0f)
            : 0.0f;
        if (blendRadius <= 0.0f || distance >= blendRadius)
            return 0.0f;
        return std::clamp(1.0f - distance / blendRadius, 0.0f, 1.0f);
    }

    RenderPostProcessSettings ResolvePostProcessSettings(
        const PostProcessResolveInput& input)
    {
        RenderPostProcessSettings result = input.ProjectDefaults;
        result.Exposure = SanitizeExposure(result.Exposure, 0.8f);
        if (input.EnvironmentExposureOverride)
        {
            result.Exposure = SanitizeExposure(
                *input.EnvironmentExposureOverride,
                result.Exposure);
        }

        std::vector<const PostProcessVolumeSnapshot*> sortedVolumes;
        sortedVolumes.reserve(input.Volumes.size());
        for (const PostProcessVolumeSnapshot& volume : input.Volumes)
            sortedVolumes.push_back(&volume);
        std::sort(
            sortedVolumes.begin(),
            sortedVolumes.end(),
            [](const PostProcessVolumeSnapshot* lhs,
                const PostProcessVolumeSnapshot* rhs)
            {
                const float lhsPriority = SanitizePriority(lhs->Priority);
                const float rhsPriority = SanitizePriority(rhs->Priority);
                if (lhsPriority != rhsPriority)
                    return lhsPriority < rhsPriority;
                return lhs->StableEntityId < rhs->StableEntityId;
            });

        for (const PostProcessVolumeSnapshot* volume : sortedVolumes)
        {
            if (!volume->bOverrideExposure)
                continue;
            const float influence = ComputePostProcessVolumeInfluence(
                *volume,
                input.CameraPosition);
            const float blendWeight = std::isfinite(volume->BlendWeight)
                ? std::clamp(volume->BlendWeight, 0.0f, 1.0f)
                : 0.0f;
            const float weight = influence * blendWeight;
            if (weight <= 0.0f || !std::isfinite(volume->Settings.Exposure))
                continue;
            const float targetExposure = std::max(
                volume->Settings.Exposure,
                0.0f);
            result.Exposure += (targetExposure - result.Exposure) * weight;
        }

        if (input.CameraExposureOverride)
        {
            result.Exposure = SanitizeExposure(
                *input.CameraExposureOverride,
                result.Exposure);
        }
        return result;
    }
}
