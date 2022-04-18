#pragma once

#include <glm/glm.hpp>

namespace HEngine
{
    class Camera
    {
    public:
        Camera() = default;
        Camera(const glm::mat4& projection)
            : m_Projection(projection) {}

        virtual ~Camera() = default;

        [[nodiscard]] const glm::mat4& GetProjection() const { return m_Projection; }
    protected:
        glm::mat4 m_Projection = glm::mat4(1.0f);
    };
}