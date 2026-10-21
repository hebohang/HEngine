#pragma once

#include <Effekseer.h>
#include <glm/glm.hpp>

#include <cmath>
#include <stdexcept>

namespace HEngine
{
    inline constexpr auto EffekseerSceneCoordinateSystem = Effekseer::CoordinateSystem::RH;

    [[nodiscard]] inline Effekseer::Matrix44 ToEffekseerCameraMatrix(const glm::mat4& matrix)
    {
        Effekseer::Matrix44 result;
        // GLM column-vector storage becomes Effekseer's row-vector matrix.
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                result.Values[row][column] = matrix[row][column];
        return result;
    }

    [[nodiscard]] inline Effekseer::Manager::DrawParameter BuildEffekseerCameraDrawParameters(
        const glm::mat4& view, const glm::mat4& projection,
        float nearDistance, float farDistance, const glm::vec3& position)
    {
        Effekseer::Manager::DrawParameter result;
        // The renderer refreshes its cached product only in BeginRendering().
        result.ViewProjectionMatrix = ToEffekseerCameraMatrix(projection * view);
        // Effekseer ZNear/ZFar are NDC coordinates, not view-space distances.
        const auto projectedDepth = [&projection](float distance)
        {
            const glm::vec4 clip = projection * glm::vec4(0.0f, 0.0f, -distance, 1.0f);
            if (!std::isfinite(clip.z) || !std::isfinite(clip.w) || std::abs(clip.w) < 1e-12f)
                throw std::invalid_argument("Effekseer camera clip distances cannot be projected");
            return clip.z / clip.w;
        };
        result.ZNear = projectedDepth(nearDistance);
        result.ZFar = projectedDepth(farDistance);
        const glm::vec3 forward = -glm::normalize(glm::vec3(glm::inverse(view)[2]));
        result.CameraFrontDirection = { forward.x, forward.y, forward.z };
        result.CameraPosition = { position.x, position.y, position.z };
        return result;
    }
}
