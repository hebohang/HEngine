#pragma once

#include <glm/glm.hpp>

#include <cmath>
#include <stdexcept>

namespace HEngine
{
    // Reproject an infinite-distance direction. Subtract the current camera
    // position in homogeneous space, then set w=0 so previous translation has
    // no effect. This also works with an infinite far plane (input world w=0).
    [[nodiscard]] inline glm::mat4 BuildSkyClipReprojection(
        const glm::mat4& inverseCurrentViewProjection,
        const glm::mat4& previousViewProjection,
        const glm::vec3& currentPosition)
    {
        glm::mat4 directionFromWorld(1.0f);
        directionFromWorld[3] = glm::vec4(-currentPosition, 0.0f);
        return previousViewProjection * directionFromWorld * inverseCurrentViewProjection;
    }

    // Camera clip distances are world distances. Inherited scale/shear must
    // affect the camera's position and aim, never scale its view-space metres.
    [[nodiscard]] inline glm::mat4 BuildRigidCameraView(const glm::mat4& world)
    {
        const glm::vec3 position(world[3]);
        glm::vec3 backward(world[2]);
        glm::vec3 right = glm::cross(glm::vec3(world[1]), backward);
        const float backwardLength = glm::length(backward);
        const float rightLength = glm::length(right);
        if (!std::isfinite(backwardLength) || !std::isfinite(rightLength) ||
            backwardLength < 1e-12f || rightLength < 1e-12f ||
            !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
            throw std::invalid_argument("Camera world transform has no finite, non-degenerate orientation");
        backward /= backwardLength;
        right /= rightLength;
        const glm::vec3 up = glm::cross(backward, right);
        return glm::mat4(
            glm::vec4(right.x, up.x, backward.x, 0.0f),
            glm::vec4(right.y, up.y, backward.y, 0.0f),
            glm::vec4(right.z, up.z, backward.z, 0.0f),
            glm::vec4(-glm::dot(right, position), -glm::dot(up, position),
                -glm::dot(backward, position), 1.0f));
    }
}
