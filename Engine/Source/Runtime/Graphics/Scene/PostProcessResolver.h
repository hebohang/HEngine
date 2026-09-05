#pragma once

#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

namespace HEngine
{
    // Immutable ECS-free input captured before RenderGraph construction.
    // WorldTransform maps a unit box in [-0.5, 0.5] to world space.
    struct PostProcessVolumeSnapshot
    {
        std::string StableEntityId;
        glm::mat4 WorldTransform{ 1.0f };
        RenderPostProcessSettings Settings;
        float Priority = 0.0f;
        float BlendRadius = 0.0f;
        float BlendWeight = 1.0f;
        bool bEnabled = true;
        bool bGlobal = true;
        bool bLocalVolumeEnabled = false;
        bool bOverrideExposure = true;
    };

    struct PostProcessResolveInput
    {
        RenderPostProcessSettings ProjectDefaults;
        std::optional<float> EnvironmentExposureOverride;
        std::vector<PostProcessVolumeSnapshot> Volumes;
        glm::vec3 CameraPosition{ 0.0f };
        std::optional<float> CameraExposureOverride;
    };

    // Global volumes have full spatial influence. Local volumes use the
    // transformed unit box plus an outer world-space blend radius.
    [[nodiscard]] float ComputePostProcessVolumeInfluence(
        const PostProcessVolumeSnapshot& volume,
        const glm::vec3& cameraPosition) noexcept;

    // Resolves Project -> Environment world override -> sorted Volumes ->
    // Camera. Volumes are applied from low to high priority, with stable entity
    // ID as the deterministic tie-breaker.
    [[nodiscard]] RenderPostProcessSettings ResolvePostProcessSettings(
        const PostProcessResolveInput& input);
}
