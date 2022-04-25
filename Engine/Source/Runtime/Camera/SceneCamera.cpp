#include "hepch.h"

#include "Runtime/Camera/SceneCamera.h"

#include <glm/gtc/matrix_transform.hpp>

namespace HEngine
{
    SceneCamera::SceneCamera()
    {
        RecalculateProjection();
    }

    void SceneCamera::SetOrthographic(float size, float nearClip, float farClip)
    {
        mProjectionType = ProjectionType::Orthographic;
        mOrthographicSize = size;
        mOrthographicNear = farClip;
        mOrthographicFar  = nearClip;
        RecalculateProjection();
    }

    void SceneCamera::SetPerspective(float verticalFOV, float nearClip, float farClip)
    {
        mProjectionType = ProjectionType::Perspective;
        mPerspectiveFOV = verticalFOV;
        mPerspectiveNear = nearClip;
        mPerspectiveFar = farClip;
        RecalculateProjection();
    }

    void SceneCamera::SetViewportSize(uint32_t width, uint32_t height)
    {
        mAspectRatio = (float)width / (float)height;
        RecalculateProjection();
    }

    void SceneCamera::RecalculateProjection()
    {
        if (mProjectionType == ProjectionType::Perspective)
        {
            mProjection = glm::perspective(mPerspectiveFOV, mAspectRatio, mPerspectiveNear, mPerspectiveFar);
        }
        else
        {
            float orthoLeft = -mOrthographicSize * mAspectRatio * 0.5f;
            float orthoRight = mOrthographicSize * mAspectRatio * 0.5f;
            float orthoBottom = -mOrthographicSize * 0.5f;
            float orthoTop = mOrthographicSize * 0.5f;

            mProjection = glm::ortho(orthoLeft, orthoRight,
                orthoBottom, orthoTop, mOrthographicNear, mOrthographicFar);
        }
    }
}
