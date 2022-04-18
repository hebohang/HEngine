#pragma once

#include <glm/glm.hpp>

namespace HEngine
{
    class OrthographicCamera
    {
    public:
        OrthographicCamera(float left, float right, float bottom, float top);

        void SetProjection(float left, float right, float bottom, float top);

        [[nodiscard]] const glm::vec3& GetPosition() const { return mPosition; }
        void SetPosition(const glm::vec3& position) { mPosition = position; RecalculateViewMatrix(); }

        [[nodiscard]] float GetRotation() const { return mRotation; }
        void SetRotation(float rotation) { mRotation = rotation; RecalculateViewMatrix(); }

        [[nodiscard]]const glm::mat4& GetProjectionMatrix() const { return mProjectionMatrix; }
        [[nodiscard]]const glm::mat4& GetViewMatrix() const { return mViewMatrix; }
        [[nodiscard]]const glm::mat4& GetViewProjectionMatrix() const { return mViewProjectionMatrix; }
    private:
        void RecalculateViewMatrix();
    private:
        glm::mat4 mProjectionMatrix;
        glm::mat4 mViewMatrix;
        glm::mat4 mViewProjectionMatrix;

        glm::vec3 mPosition = { 0.0f, 0.0f, 0.0f };
        float mRotation = 0.0f;
    };
}