#include "hepch.h"
#include "OrthographicCameraController.h"

#include "Runtime/Input/Input.h"
#include "Runtime/Input/KeyCodes.h"

namespace HEngine
{
    OrthographicCameraController::OrthographicCameraController(float aspectRatio, bool rotation)
        : mAspectRatio(aspectRatio), mCamera(-mAspectRatio * mZoomLevel, mAspectRatio* mZoomLevel, -mZoomLevel, mZoomLevel), mRotation(rotation)
    {
    }

    void OrthographicCameraController::OnUpdate(Timestep ts)
    {
        if (Input::IsKeyPressed(HE_KEY_A))
            mCameraPosition.x -= mCameraTranslationSpeed * ts;
        else if (Input::IsKeyPressed(HE_KEY_D))
            mCameraPosition.x += mCameraTranslationSpeed * ts;

        if (Input::IsKeyPressed(HE_KEY_W))
            mCameraPosition.y += mCameraTranslationSpeed * ts;
        else if (Input::IsKeyPressed(HE_KEY_S))
            mCameraPosition.y -= mCameraTranslationSpeed * ts;

        if (mRotation)
        {
            if (Input::IsKeyPressed(HE_KEY_Q))
                mCameraRotation += mCameraRotationSpeed * ts;
            else if (Input::IsKeyPressed(HE_KEY_E))
                mCameraRotation -= mCameraRotationSpeed * ts;

            if (mCameraRotation > 180.0f)
                mCameraRotation -= 360.0f;
            else if (mCameraRotation <= -180.0f)
                mCameraRotation += 360.0f;

            mCamera.SetRotation(mCameraRotation);
        }

        mCamera.SetPosition(mCameraPosition);

        mCameraTranslationSpeed = mZoomLevel;
    }

    void OrthographicCameraController::OnEvent(Event& e)
    {
        EventDispatcher dispatcher(e);
        dispatcher.Dispatch<MouseScrolledEvent>(HE_BIND_EVENT_FN(OrthographicCameraController::OnMouseScrolled));
        dispatcher.Dispatch<WindowResizeEvent>(HE_BIND_EVENT_FN(OrthographicCameraController::OnWindowResized));
    }

    void OrthographicCameraController::OnResize(float width, float height)
    {
        mAspectRatio = width / height;
        mCamera.SetProjection(-mAspectRatio * mZoomLevel, mAspectRatio * mZoomLevel, -mZoomLevel, mZoomLevel);
    }

    bool OrthographicCameraController::OnMouseScrolled(MouseScrolledEvent& e)
    {
        mZoomLevel -= e.GetYOffset() * 0.25f;
        mZoomLevel = std::max(mZoomLevel, 0.25f);
        mCamera.SetProjection(-mAspectRatio * mZoomLevel, mAspectRatio * mZoomLevel, -mZoomLevel, mZoomLevel);
        return false;
    }

    bool OrthographicCameraController::OnWindowResized(WindowResizeEvent& e)
    {
        OnResize((float)e.GetWidth(), (float)e.GetHeight());
        return false;
    }
}