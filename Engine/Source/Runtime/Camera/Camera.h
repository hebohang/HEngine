#pragma once
#include "Runtime/Core/Meta/Reflection/Reflection.h"

#include <glm/glm.hpp>

namespace HEngine
{
    REFLECTION_TYPE(TransformComponent)
    CLASS(Camera, WhiteListFields)
    {
    public:
        Camera() = default;
        Camera(const glm::mat4& projection)
            : mProjection(projection) {}

        virtual ~Camera() = default;

        [[nodiscard]] const glm::mat4& GetProjection() const { return mProjection; }
    protected:
        glm::mat4 mProjection = glm::mat4(1.0f);
    };
}