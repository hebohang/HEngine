#pragma once

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace HEngine::DirectionalShadowMath
{
    inline constexpr float CascadeBlendFraction = 0.20f;
    inline constexpr float FarFadeFraction = 0.15f;
    inline constexpr float CasterDepthPadding = 20.0f;
    // Covers the maximum PCF half-width and the half-texel origin snap.
    inline constexpr float FilterGuardTexels = 5.0f;

    inline float SanitizeNearPlane(float nearPlane) noexcept
    {
        return std::isfinite(nearPlane) ? std::max(nearPlane, 0.001f) : 0.001f;
    }

    inline float ComputeSplit(float nearPlane, float farPlane, float ratio, float lambda)
    {
        const float logarithmicSplit = nearPlane * std::pow(farPlane / nearPlane, ratio);
        const float uniformSplit = nearPlane + (farPlane - nearPlane) * ratio;
        return glm::mix(uniformSplit, logarithmicSplit, lambda);
    }

    inline float ComputeSliceNear(
        uint32_t cascade, float nearPlane, const std::array<float, 4>& splits)
    {
        if (cascade == 0)
            return nearPlane;
        const float previousNear = cascade == 1 ? nearPlane : splits[cascade - 2];
        return glm::mix(splits[cascade - 1], previousNear, CascadeBlendFraction);
    }

    inline std::array<glm::vec3, 8> ComputeViewFrustumCorners(
        const glm::mat4& projection, float nearSplit, float farSplit)
    {
        const glm::mat4 inverseProjection = glm::inverse(projection);
        const glm::vec4 projectedNear = projection * glm::vec4(0, 0, -nearSplit, 1);
        const glm::vec4 projectedFar = projection * glm::vec4(0, 0, -farSplit, 1);
        const float ndcNear = projectedNear.z / projectedNear.w;
        const float ndcFar = projectedFar.z / projectedFar.w;
        std::array<glm::vec3, 8> corners{};
        uint32_t cornerIndex = 0;
        for (float x : { -1.0f, 1.0f })
            for (float y : { -1.0f, 1.0f })
                for (float z : { ndcNear, ndcFar })
                {
                    const glm::vec4 corner = inverseProjection * glm::vec4(x, y, z, 1);
                    corners[cornerIndex++] = glm::vec3(corner) / corner.w;
                }
        return corners;
    }

    struct Sphere
    {
        glm::vec3 Center{ 0.0f };
        float Radius = 0.0f;
    };

    inline Sphere FitFrustumSphere(const std::array<glm::vec3, 8>& corners)
    {
        glm::vec3 nearCenter(0), farCenter(0);
        for (uint32_t corner = 0; corner < 8; corner += 2)
        {
            nearCenter += corners[corner] * 0.25f;
            farCenter += corners[corner + 1] * 0.25f;
        }
        const glm::vec3 axis = farCenter - nearCenter;
        const float distanceSquared = glm::dot(axis, axis);
        float nearRadiusSquared = 0, farRadiusSquared = 0;
        for (uint32_t corner = 0; corner < 8; corner += 2)
        {
            const glm::vec3 nearOffset = corners[corner] - nearCenter;
            const glm::vec3 farOffset = corners[corner + 1] - farCenter;
            nearRadiusSquared = std::max(nearRadiusSquared, glm::dot(nearOffset, nearOffset));
            farRadiusSquared = std::max(farRadiusSquared, glm::dot(farOffset, farOffset));
        }
        // The two plane radii determine the minimax sphere along the view axis.
        // Fit in view space so camera rotation/translation cannot resize it.
        const float fraction = distanceSquared > 1e-8f
            ? std::clamp((distanceSquared + farRadiusSquared - nearRadiusSquared) /
                (2.0f * distanceSquared), 0.0f, 1.0f)
            : 0.5f;
        Sphere sphere{ nearCenter + axis * fraction, 0.0f };
        for (const glm::vec3& corner : corners)
            sphere.Radius = std::max(sphere.Radius, glm::length(corner - sphere.Center));
        return sphere;
    }

    inline glm::mat4 ComputeCascadeMatrix(
        const glm::mat4& view, const glm::mat4& projection,
        float nearSplit, float farSplit, const glm::vec3& lightDirection,
        uint32_t shadowMapSize)
    {
        const Sphere sphere = FitFrustumSphere(
            ComputeViewFrustumCorners(projection, nearSplit, farSplit));
        const glm::vec3 center = glm::vec3(glm::inverse(view) * glm::vec4(sphere.Center, 1));
        const float guardScale = 1.0f - 2.0f * FilterGuardTexels /
            std::max(static_cast<float>(shadowMapSize), 32.0f);
        // Quantize only after adding the filter gutter. Rotation keeps exactly
        // the same extent; snapping moves the projection in whole world texels.
        const float radius = std::max(std::ceil(sphere.Radius / guardScale * 16.0f) /
            16.0f, 1.0f / 16.0f);
        const glm::vec3 up = std::abs(glm::dot(lightDirection, glm::vec3(0, 1, 0))) > 0.95f
            ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        const glm::mat4 lightView = glm::lookAt(
            center - lightDirection * (radius * 2.0f + CasterDepthPadding), center, up);
        glm::mat4 lightProjection = glm::orthoZO(-radius, radius, -radius, radius,
            0.0f, radius * 4.0f + CasterDepthPadding * 2.0f);
        const float halfResolution = std::max(static_cast<float>(shadowMapSize) * 0.5f, 1.0f);
        const glm::vec2 origin = glm::vec2(lightProjection * lightView * glm::vec4(0, 0, 0, 1)) *
            halfResolution;
        const glm::vec2 offset = (glm::round(origin) - origin) / halfResolution;
        lightProjection[3][0] += offset.x;
        lightProjection[3][1] += offset.y;
        return lightProjection * lightView;
    }
}
