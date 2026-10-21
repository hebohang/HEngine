#include "Hepch.h"

#include "Runtime/Graphics/Scene/PostProcessResolver.h"
#include "Runtime/Graphics/Settings/RenderSettings.h"

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
        static const auto* bloomDescriptor = FindRenderSettingDescriptor(RenderSettingIds::BloomIntensity);
        static const auto* contrastDescriptor = FindRenderSettingDescriptor(RenderSettingIds::ColorContrast);
        static const auto* saturationDescriptor = FindRenderSettingDescriptor(RenderSettingIds::ColorSaturation);
        static const auto* temperatureDescriptor = FindRenderSettingDescriptor(RenderSettingIds::ColorTemperature);
        const auto normalize = [](float value, float fallback, const RenderSettingDescriptor* descriptor)
        {
            if (!std::isfinite(value)) return fallback;
            if (descriptor && descriptor->Minimum) value = std::max(value, static_cast<float>(*descriptor->Minimum));
            if (descriptor && descriptor->Maximum) value = std::min(value, static_cast<float>(*descriptor->Maximum));
            return value;
        };
        const auto normalizeColor = [](glm::vec3 value, glm::vec3 fallback, float minimum, float maximum)
        {
            for (int channel = 0; channel < 3; ++channel)
                value[channel] = std::isfinite(value[channel])
                    ? std::clamp(value[channel], minimum, maximum) : fallback[channel];
            return value;
        };
        result.BloomIntensity = normalize(result.BloomIntensity, 0.05f, bloomDescriptor);
        result.Contrast = normalize(result.Contrast, 1.0f, contrastDescriptor);
        result.Saturation = normalize(result.Saturation, 1.0f, saturationDescriptor);
        result.Temperature = normalize(result.Temperature, 0.0f, temperatureDescriptor);
        result.VignetteIntensity = std::clamp(std::isfinite(result.VignetteIntensity)
            ? result.VignetteIntensity : 0.0f, 0.0f, 1.0f);
        result.ColorGain = normalizeColor(result.ColorGain, glm::vec3(1.0f), 0.0f, 2.0f);
        result.ColorOffset = normalizeColor(result.ColorOffset, glm::vec3(0.0f), -0.25f, 0.25f);
        result.BloomBaseWeight = result.ContrastBaseWeight = result.SaturationBaseWeight =
            result.TemperatureBaseWeight = result.VignetteBaseWeight = 1.0f;
        result.BloomContribution = result.ContrastContribution = result.SaturationContribution =
            result.TemperatureContribution = result.VignetteContribution = 0.0f;
        result.ColorGainBaseWeight = result.ColorOffsetBaseWeight = 1.0f;
        result.ColorGainContribution = result.ColorOffsetContribution = glm::vec3(0.0f);
        result.Exposure = SanitizeExposure(result.Exposure, 1.0f / 16384.0f);
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
            const float influence = ComputePostProcessVolumeInfluence(
                *volume,
                input.CameraPosition);
            const float blendWeight = std::isfinite(volume->BlendWeight)
                ? std::clamp(volume->BlendWeight, 0.0f, 1.0f)
                : 0.0f;
            const float weight = influence * blendWeight;
            if (weight <= 0.0f) continue;
            if (volume->bOverrideExposure && std::isfinite(volume->Settings.Exposure))
                result.Exposure += (std::max(volume->Settings.Exposure, 0.0f) - result.Exposure) * weight;
            const auto blend = [&](bool enabled, float target, float& value, float& baseWeight,
                float& contribution, const RenderSettingDescriptor* descriptor)
            {
                if (!enabled || !std::isfinite(target)) return;
                target = normalize(target, value, descriptor);
                value += (target - value) * weight;
                baseWeight *= 1.0f - weight;
                contribution += (target - contribution) * weight;
            };
            blend(volume->bOverrideBloomIntensity, volume->Settings.BloomIntensity, result.BloomIntensity,
                result.BloomBaseWeight, result.BloomContribution, bloomDescriptor);
            blend(volume->bOverrideContrast, volume->Settings.Contrast, result.Contrast,
                result.ContrastBaseWeight, result.ContrastContribution, contrastDescriptor);
            blend(volume->bOverrideSaturation, volume->Settings.Saturation, result.Saturation,
                result.SaturationBaseWeight, result.SaturationContribution, saturationDescriptor);
            blend(volume->bOverrideTemperature, volume->Settings.Temperature, result.Temperature,
                result.TemperatureBaseWeight, result.TemperatureContribution, temperatureDescriptor);
            blend(volume->bOverrideVignetteIntensity,
                std::clamp(volume->Settings.VignetteIntensity, 0.0f, 1.0f), result.VignetteIntensity,
                result.VignetteBaseWeight, result.VignetteContribution, nullptr);
            const auto blendColor = [&](bool enabled, glm::vec3 target, glm::vec3& value,
                float& baseWeight, glm::vec3& contribution, float minimum, float maximum)
            {
                if (!enabled || !std::isfinite(target.x) || !std::isfinite(target.y) ||
                    !std::isfinite(target.z)) return;
                target = normalizeColor(target, value, minimum, maximum);
                value += (target - value) * weight;
                baseWeight *= 1.0f - weight;
                contribution += (target - contribution) * weight;
            };
            blendColor(volume->bOverrideColorGain, volume->Settings.ColorGain, result.ColorGain,
                result.ColorGainBaseWeight, result.ColorGainContribution, 0.0f, 2.0f);
            blendColor(volume->bOverrideColorOffset, volume->Settings.ColorOffset, result.ColorOffset,
                result.ColorOffsetBaseWeight, result.ColorOffsetContribution, -0.25f, 0.25f);
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
