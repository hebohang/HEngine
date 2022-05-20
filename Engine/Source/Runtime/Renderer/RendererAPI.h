#pragma once

#include "Runtime/Renderer/VertexArray.h"

#include <glm/glm.hpp>

namespace HEngine
{
    //stencil func and op
    enum class StencilFunc
    {
        ALWAYS,
        NOTEQUAL
    };

    enum class StencilOp
    {
        KEEP,
        INCREMENT,
        DECREMENT
    };

    enum class DepthComp
    {
        EQUAL,
        LEQUAL,
        LESS
    };

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

        //depth
        virtual void DepthTest(bool enable) = 0;
        virtual void DepthMask(bool maskFlag) = 0;
        virtual void DepthFunc(DepthComp comp) = 0;

        //blend
        virtual void Blend(int32_t Bit) = 0;

        virtual void BindTexture(int32_t slot, uint32_t textureID) = 0;

        //stencil
        virtual void SetStencilFunc(StencilFunc stencilFunc, int32_t ref, int32_t mask) = 0;
        virtual void SetFrontOrBackStencilOp(int32_t FrontOrBack, StencilOp stencilFail, StencilOp depthFail, StencilOp depthSuccess) = 0;
        virtual void StencilTest(int32_t Bit) = 0;
        virtual void ClearStencil() = 0;
        virtual void StencilMask(uint32_t mask) = 0;

        //cull
        virtual void Cull(int32_t Bit) = 0;
        virtual void CullFrontOrBack(int32_t Bit) = 0;

        [[nodiscard]] virtual int GetDrawFrameBuffer() = 0;

        [[nodiscard]] static RendererAPIType Current() { return sCurrentRendererAPI; }

        virtual void BindFrameBuffer(uint32_t framebufferID) = 0;
    private:
        static inline RendererAPIType sCurrentRendererAPI = RendererAPIType::OpenGL;
    };
}
