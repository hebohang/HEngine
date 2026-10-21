#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/matrix_decompose.hpp>

namespace HEngine
{
    struct LocalPose
    {
        glm::vec3 Translation{0.0f};
        glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 Scale{1.0f};

        static LocalPose FromMatrix(const glm::mat4& matrix)
        {
            LocalPose pose;
            glm::vec3 skew;
            glm::vec4 perspective;
            if (!glm::decompose(matrix, pose.Scale, pose.Rotation, pose.Translation, skew, perspective))
                throw std::invalid_argument("Animation bind pose has a singular transform.");
            pose.Rotation = glm::normalize(pose.Rotation);
            return pose;
        }

        glm::mat4 Matrix() const
        {
            return glm::translate(glm::mat4(1.0f), Translation) * glm::mat4_cast(Rotation) *
                glm::scale(glm::mat4(1.0f), Scale);
        }

        static LocalPose Blend(const LocalPose& from, const LocalPose& to, float weight)
        {
            if (!std::isfinite(weight))
                throw std::invalid_argument("Animation blend weight must be finite.");
            const float alpha = std::clamp(weight, 0.0f, 1.0f);
            return {glm::mix(from.Translation, to.Translation, alpha),
                glm::normalize(glm::slerp(from.Rotation, to.Rotation, alpha)),
                glm::mix(from.Scale, to.Scale, alpha)};
        }
    };
}
