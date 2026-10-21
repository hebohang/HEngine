#pragma once

#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace HEngine::Math
{
    // Matches glm::quat(vec3): Rz * Ry * Rx. Use double precision and atan2
    // instead of asin(yaw), which loses precision near the +/-90 degree poles.
    // At the pole pitch/roll are coupled; choose roll=0 consistently. Input must
    // be a finite, nonzero rotation quaternion.
    [[nodiscard]] inline glm::vec3 StableEulerAngles(const glm::quat& orientation)
    {
        const glm::dmat3 rotation = glm::mat3_cast(glm::normalize(glm::dquat(orientation)));
        const double cosineYaw = std::hypot(rotation[0][0], rotation[0][1]);
        const double yaw = std::atan2(-rotation[0][2], cosineYaw);
        if (cosineYaw < 1e-6)
            return glm::vec3(std::atan2(-rotation[2][1], rotation[1][1]), yaw, 0.0);
        return glm::vec3(std::atan2(rotation[1][2], rotation[2][2]), yaw,
            std::atan2(rotation[0][1], rotation[0][0]));
    }
}
