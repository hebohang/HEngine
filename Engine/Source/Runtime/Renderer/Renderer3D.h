#pragma once

#include "Runtime/Renderer/Camera.h"
#include "Runtime/Renderer/EditorCamera.h"

namespace HEngine
{
    class Renderer3D
    {
    public:
        static void Init();
        static void Shutdown();

        static void BeginScene(const Camera& camera, const glm::mat4& transform);
        static void BeginScene(const EditorCamera& camera);
        static void EndScene();

        static void DrawCube(const glm::mat4& transform);
    };
}