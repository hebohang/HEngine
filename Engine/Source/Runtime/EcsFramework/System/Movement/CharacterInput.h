#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace HEngine
{
    // Preserve analog strength; only over-length (e.g. keyboard diagonal) input
    // is normalized. The camera basis is horizontal and orthonormal.
    inline glm::vec3 CharacterInputVelocity(glm::vec2 input, glm::vec3 right,
        glm::vec3 forward, float maxSpeed)
    {
        const float length = glm::length(input);
        if (!std::isfinite(length) || length == 0.0f) return glm::vec3(0.0f);
        input /= std::max(1.0f, length);
        return (right * input.x + forward * input.y) * maxSpeed;
    }
}
