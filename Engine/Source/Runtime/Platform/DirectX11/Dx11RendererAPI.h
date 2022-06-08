#pragma once

#include "Runtime/Renderer/RendererAPI.h"

namespace HEngine
{
    class Dx11RendererAPI : public RendererAPI
    {
    public:
        virtual void Init() override;
        virtual void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;

        virtual void SetClearColor(const glm::vec4& color) override;
        virtual void Clear() override;

        virtual void DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t indexCount = 0) override;
        virtual void DrawLines(const Ref<VertexArray>& vertexArray, uint32_t vertexCount) override;

        virtual void SetLineWidth(float width) override;

        //depth
        virtual void DepthTest(bool enable) override {};
        virtual void DepthMask(bool maskFlag) override {};
        virtual void DepthFunc(DepthComp comp) override {};

        //blend
        virtual void Blend(int32_t Bit) override {};

        virtual void BindTexture(int32_t slot, uint32_t textureID) override {};

        //stencil
        virtual void SetStencilFunc(StencilFunc stencilFunc, int32_t ref, int32_t mask) override {};
        virtual void SetFrontOrBackStencilOp(int32_t FrontOrBack, StencilOp stencilFail, StencilOp depthFail, StencilOp depthSuccess) override {};
        virtual void StencilTest(int32_t Bit) override {};
        virtual void ClearStencil() override {};
        virtual void StencilMask(uint32_t mask) override {};

        //cull
        virtual void Cull(int32_t Bit) override {};
        virtual void CullFrontOrBack(bool bFront) override {};

        [[nodiscard]] virtual int GetDrawFrameBuffer() override { return 0; };

        virtual void BindFrameBuffer(uint32_t framebufferID) override {};
    };
}
