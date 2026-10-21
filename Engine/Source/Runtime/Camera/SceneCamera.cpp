#include "Hepch.h"

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
        mOrthographicNear = nearClip;
        mOrthographicFar  = farClip;
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
        if (width && height) SetAspectRatio(static_cast<float>(width) / height);
    }

    void SceneCamera::SetAspectRatio(float aspect)
    {
        mAspectRatio = std::isfinite(aspect) && aspect > 0.0f ? aspect : 1.0f;
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

            // Both supported render backends use device depth [0,1]. The
            // default GLM orthographic [-1,1] projection clips the near half
            // of a 2D scene before rasterization (for example z=-10 at 0.1..100).
            mProjection = glm::orthoRH_ZO(orthoLeft, orthoRight,
                orthoBottom, orthoTop, mOrthographicNear, mOrthographicFar);
        }
    }
}
