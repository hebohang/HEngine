#pragma once

#include "Runtime/Renderer/VertexArray.h"

#include <glm/glm.hpp>

namespace HEngine
{
    class RendererAPI
    {
    public:
        enum class RendererAPIType
        {
            None = 0, 
            OpenGL, 
            Vulkan,
            DX11,
            DX12,
        };
    public:
        virtual void Init() = 0;
        virtual void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;
        virtual void SetClearColor(const glm::vec4& color) = 0;
        virtual void Clear() = 0;

        virtual void DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t indexCount = 0) = 0;
		virtual void DrawLines(const Ref<VertexArray>& vertexArray, uint32_t vertexCount) = 0;

		virtual void SetLineWidth(float width) = 0;

        [[nodiscard]] static RendererAPIType Current() { return sCurrentRendererAPI; }
    private:
        static inline RendererAPIType sCurrentRendererAPI = RendererAPIType::Vulkan;
    };
}
