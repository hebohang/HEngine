#pragma once

#include "Runtime/Renderer/OrthographicCamera.h"
#include "Runtime/Core/Timestep.h"

#include "Runtime/Events/ApplicationEvent.h"
#include "Runtime/Events/MouseEvent.h"

namespace HEngine
{
    class OrthographicCameraController
    {
    public:
        OrthographicCameraController(float aspectRatio, bool rotation = false);

        void OnUpdate(Timestep ts);
        void OnEvent(Event& e);

        void OnResize(float width, float height);

        [[nodiscard]] OrthographicCamera& GetCamera() { return mCamera; }
        [[nodiscard]] const OrthographicCamera& GetCamera() const { return mCamera; }

        [[nodiscard]] float GetZoomLevel() const { return mZoomLevel; }
        void SetZoomLevel(float level) { mZoomLevel = level; }
    private:
        bool OnMouseScrolled(MouseScrolledEvent& e);
        bool OnWindowResized(WindowResizeEvent& e);
    private:
        float mAspectRatio;
        float mZoomLevel = 1.0f;
        OrthographicCamera mCamera;

        bool mRotation = false;

        glm::vec3 mCameraPosition = { 0.0f, 0.0f, 0.0f };
        float mCameraRotation = 0.0f;
        float mCameraTranslationSpeed = 5.0f, mCameraRotationSpeed = 180.0f;
    };
}