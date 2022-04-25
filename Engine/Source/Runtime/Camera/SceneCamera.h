#pragma once

#include "Runtime/Camera/Camera.h"

namespace HEngine
{
    class SceneCamera : public Camera
    {
    public:
        enum class ProjectionType { Perspective = 0, Orthographic = 1 };
    public:
        SceneCamera();
        virtual ~SceneCamera() = default;

        void SetOrthographic(float size, float nearClip, float farClip);
        void SetPerspective(float verticalFOV, float nearClip, float farClip);

        void SetViewportSize(uint32_t width, uint32_t height);

        float GetPerspectiveVerticalFOV() const { return mPerspectiveFOV; }
        void SetPerspectiveVerticalFOV(float verticalFOV) { mPerspectiveFOV = verticalFOV; RecalculateProjection(); }
        float GetPerspectiveNearClip() const { return mPerspectiveNear; }
        void SetPerspectiveNearClip(float nearClip) { mPerspectiveNear = nearClip; RecalculateProjection(); }
        float GetPerspectiveFarClip() const { return mPerspectiveFar; }
        void SetPerspectiveFarClip(float farClip) { mPerspectiveFar = farClip; RecalculateProjection(); }

        float GetOrthographicSize() const { return mOrthographicSize; }
        void SetOrthographicSize(float size) { mOrthographicSize = size; RecalculateProjection(); }
        float GetOrthographicNearClip() const { return mOrthographicNear; }
        void SetOrthographicNearClip(float nearClip) { mOrthographicNear = nearClip; RecalculateProjection(); }
        float GetOrthographicFarClip() const { return mOrthographicFar; }
        void SetOrthographicFarClip(float farClip) { mOrthographicFar = farClip; RecalculateProjection(); }

        ProjectionType GetProjectionType() const { return mProjectionType; }
        void SetProjectionType(ProjectionType type) { mProjectionType = type; }
    private:
        void RecalculateProjection();
    private:
        ProjectionType mProjectionType = ProjectionType::Orthographic;

        float mPerspectiveFOV = glm::radians(45.0f);
        float mPerspectiveNear = 0.01f, mPerspectiveFar = 1000.0f;

        float mOrthographicSize = 10.0f;
        float mOrthographicNear = -1.0f, mOrthographicFar = 1.0f;

        float mAspectRatio = 0.0f;
    };
}
