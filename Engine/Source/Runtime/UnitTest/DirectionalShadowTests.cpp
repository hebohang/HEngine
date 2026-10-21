#include "Hepch.h"

#include "Runtime/UnitTest/DirectionalShadowTests.h"

#include "Runtime/Graphics/Renderer/DirectionalShadowMath.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

// Exercise the actual shader scalar functions, not a separately maintained
// CPU approximation. Matrix scale extraction is validated below with GLM.
namespace HEngine::Tests::DirectionalShadowShaderMath
{
    using std::max;
#include "../../../Content/Shaders/Modern/DirectionalShadowSampling.hlsli"
}

namespace HEngine::Tests
{
    namespace
    {
        void Require(bool condition, const char* message)
        {
            if (!condition)
                throw std::runtime_error(message);
        }

        float RowScale(const glm::mat4& matrix, uint32_t row)
        {
            return glm::length(glm::vec3(matrix[0][row], matrix[1][row], matrix[2][row]));
        }
    }

    void DirectionalShadowMathContract()
    {
        using namespace DirectionalShadowMath;
        using namespace DirectionalShadowShaderMath;
        Require(SanitizeNearPlane(std::numeric_limits<float>::quiet_NaN()) == 0.001f,
            "Nonfinite camera near plane must be sanitized.");
        const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 16.0f / 9.0f,
            0.1f, 300.0f);
        const glm::vec3 lightDirection = glm::normalize(glm::vec3(0.3f, -1.0f, 0.4f));
        const glm::mat4 view = glm::lookAt(glm::vec3(4, 2, 8), glm::vec3(4, 2, 7),
            glm::vec3(0, 1, 0));
        std::array<float, 4> splits{};
        float previousSplit = 0.1f;
        for (uint32_t cascade = 0; cascade < 4; ++cascade)
        {
            splits[cascade] = ComputeSplit(0.1f, 80.0f, float(cascade + 1) / 4.0f, 0.75f);
            Require(splits[cascade] > previousSplit, "CSM splits must be strictly ordered.");
            previousSplit = splits[cascade];
        }
        Require(std::abs(splits[3] - 80.0f) < 1e-4f, "Last cascade must reach the authored distance.");
        float referenceDepthScale = 0, referenceTexelScale = 0;
        for (uint32_t cascade = 0; cascade < 4; ++cascade)
        {
            const float sliceNear = ComputeSliceNear(cascade, 0.1f, splits);
            if (cascade > 0)
            {
                const float previousNear = cascade == 1 ? 0.1f : splits[cascade - 2];
                const float blendStart = splits[cascade - 1] -
                    (splits[cascade - 1] - previousNear) * CascadeBlendFraction;
                Require(std::abs(sliceNear - blendStart) < 1e-5f,
                    "Next cascade must cover the entire shader blend interval.");
            }
            const auto corners = ComputeViewFrustumCorners(projection, sliceNear, splits[cascade]);
            const Sphere sphere = FitFrustumSphere(corners);
            glm::vec3 oldCenter(0);
            for (const glm::vec3& corner : corners)
                oldCenter += corner / 8.0f;
            float oldRadius = 0;
            for (const glm::vec3& corner : corners)
                oldRadius = std::max(oldRadius, glm::length(corner - oldCenter));
            Require(sphere.Radius <= oldRadius + 1e-4f,
                "Minimax frustum fit must not waste more texels than the previous midpoint fit.");
            const glm::mat4 matrix = ComputeCascadeMatrix(view, projection, sliceNear,
                splits[cascade], lightDirection, 2048);
            const glm::mat4 inverseView = glm::inverse(view);
            const float guardNdc = 2.0f * (FilterGuardTexels - 0.5f) / 2048.0f;
            for (const glm::vec3& corner : corners)
            {
                Require(glm::length(corner - sphere.Center) <= sphere.Radius + 1e-4f,
                    "Fitted sphere must contain every frustum corner.");
                const glm::vec4 clip = matrix * inverseView * glm::vec4(corner, 1);
                Require(std::abs(clip.x) <= 1.0f - guardNdc + 1e-4f &&
                    std::abs(clip.y) <= 1.0f - guardNdc + 1e-4f,
                    "PCF gutter must remain inside the snapped projection.");
                Require(clip.z >= 0.0f && clip.z <= 1.0f, "Receiver depth must remain in shadow range.");
            }
            const glm::mat4 rotatedView = glm::lookAt(glm::vec3(25, 5, -18),
                glm::vec3(26, 5.2f, -17), glm::vec3(0, 1, 0));
            const glm::mat4 rotated = ComputeCascadeMatrix(rotatedView, projection, sliceNear,
                splits[cascade], lightDirection, 2048);
            Require(std::abs(RowScale(matrix, 0) - RowScale(rotated, 0)) < 1e-5f,
                "Camera rotation must not rescale the shadow texel grid.");
            const glm::vec4 origin = matrix * glm::vec4(0, 0, 0, 1);
            Require(std::abs(origin.x * 1024 - std::round(origin.x * 1024)) < 0.01f &&
                std::abs(origin.y * 1024 - std::round(origin.y * 1024)) < 0.01f,
                "World origin must land on the integer shadow texel grid.");
            const float depthScale = RowScale(matrix, 2), texelScale = RowScale(matrix, 0);
            if (cascade == 0)
            {
                referenceDepthScale = depthScale;
                referenceTexelScale = texelScale;
            }
            const float bias = HE_DirectionalShadowDepthBias(0.0015f, depthScale, referenceDepthScale);
            Require(std::abs(bias / depthScale - 0.0015f / referenceDepthScale) < 1e-5f,
                "All cascades must use the same world-space authored depth bias.");
            const float radius = HE_DirectionalShadowFilterRadius(1.5f, texelScale, referenceTexelScale);
            Require(radius > 0 && radius <= 1.5f + 1e-5f,
                "Far cascades must retain continuous comparison without enlarging the filter.");
            Require(HE_DirectionalShadowFilterRadius(0, texelScale, referenceTexelScale) == 0,
                "Hard shadows must not silently enable PCF.");
        }
    }
}
