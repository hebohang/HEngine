#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <string>

namespace HEngine
{
    REFLECTION_TYPE(TransformComponent)
    CLASS(TransformComponent : public ComponentBase, Fields)
	{
        REFLECTION_COMPONENT_BODY(TransformComponent);
    public:
        TransformComponent() = default;
        TransformComponent(const TransformComponent&) = default;
        TransformComponent(const glm::vec3 & translation)
            : Translation(translation) {}
        TransformComponent(const glm::vec3& translation, const glm::vec3& rotation, const glm::vec3& scale)
            : Translation(translation), Rotation(rotation), Scale(scale) {}

        [[nodiscard]] glm::mat4 GetTransform() const
        {
            return glm::translate(glm::mat4(1.0f), Translation) * GetRotationMatrix() * glm::scale(glm::mat4(1.0f), Scale);
        }

        [[nodiscard]] glm::mat4 GetRotationMatrix() const
        {
            return glm::toMat4(glm::quat(Rotation));
        }

        glm::vec3 Translation = { 0.0f, 0.0f, 0.0f };
        glm::vec3 Rotation = { 0.0f, 0.0f, 0.0f };  // Euler angles (radians)
        glm::vec3 Scale = { 1.0f, 1.0f, 1.0f };
        std::string ParentUuid;
	};
}
