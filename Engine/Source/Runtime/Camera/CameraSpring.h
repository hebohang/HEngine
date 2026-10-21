#pragma once

#include <glm/glm.hpp>
#include <cmath>

namespace HEngine
{
    // Analytic critically damped target tracking. Position and velocity stay
    // continuous across stair height changes; the solution is stable at any dt.
    inline void AdvanceCameraSpring(glm::vec3& position, glm::vec3& velocity,
        const glm::vec3& target, const glm::vec3& frequency, float dt)
    {
        if (!std::isfinite(dt) || dt <= 0.0f) return;
        for (int axis = 0; axis < 3; ++axis)
        {
            const float rate = frequency[axis];
            if (!std::isfinite(rate) || rate <= 0.0f)
            { position[axis] = target[axis]; velocity[axis] = 0; continue; }
            const float offset = position[axis] - target[axis];
            const float impulse = velocity[axis] + rate * offset;
            const float decay = std::exp(-rate * dt);
            position[axis] = target[axis] + (offset + impulse * dt) * decay;
            velocity[axis] = (velocity[axis] - rate * impulse * dt) * decay;
        }
    }
}
