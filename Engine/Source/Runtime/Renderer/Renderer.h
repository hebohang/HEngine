#pragma once

#include "Runtime/Renderer/RenderCommand.h"

#include "Runtime/Renderer/Shader.h"

namespace HEngine
{
    class Renderer
    {
    public:
        static void Init();
		static void Shutdown();

        static void OnWindowResize(uint32_t width, uint32_t height);

        static void EndScene();

        static void Submit(const Ref<Shader>& shader, const Ref<VertexArray>& vertexArray, const glm::mat4& transform = glm::mat4(1.0f));
    private:
        struct SceneData
        {
            glm::mat4 ViewProjectionMatrix;
        };

        static SceneData* mSceneData;
    };
}
