#pragma once

#include "Runtime/Renderer/RendererAPI.h"

namespace HEngine
{
    class RenderCommand
    {
    public:
        static void Init()
        {
            sRendererAPI->Init();
        }

        static void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
        {
            sRendererAPI->SetViewport(x, y, width, height);
        }

        static void SetClearColor(const glm::vec4& color)
        {
            sRendererAPI->SetClearColor(color);
        }

        static void Clear()
        {
            sRendererAPI->Clear();
        }

        static void DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t indexCount = 0)
        {
            sRendererAPI->DrawIndexed(vertexArray, indexCount);
        }

		static void DrawLines(const Ref<VertexArray>& vertexArray, uint32_t vertexCount)
		{
			sRendererAPI->DrawLines(vertexArray, vertexCount);
		}

		static void SetLineWidth(float width)
		{
			sRendererAPI->SetLineWidth(width);
		}

		//Open/Close Depth Write
		static void DepthMask(bool maskFlag)
		{
			sRendererAPI->DepthMask(maskFlag);
		}

		//Open/Close Depth Test
		static void DepthTest(bool enable)
		{
			sRendererAPI->DepthTest(enable);
		}

		static void DepthFunc(DepthComp comp)
		{
			sRendererAPI->DepthFunc(comp);
		}

		//blend
		static void Blend(int32_t Bit)
		{
			sRendererAPI->Blend(Bit);
		}

		static void BindTexture(int32_t slot, uint32_t textureID)
		{
			sRendererAPI->BindTexture(slot, textureID);
		}

		//stencil
		static void StencilTest(int32_t Bit)
		{
			sRendererAPI->StencilTest(Bit);
		}

		static void SetStencilFunc(StencilFunc stencilFunc, int32_t ref, int32_t mask)
		{
			sRendererAPI->SetStencilFunc(stencilFunc, ref, mask);
		}

		static void SetFrontOrBackStencilOp(int32_t FrontOrBack, StencilOp stencilFail, StencilOp depthFail, StencilOp depthSuccess)
		{
			sRendererAPI->SetFrontOrBackStencilOp(FrontOrBack, stencilFail, depthFail, depthSuccess);
		}

		static void ClearStencil()
		{
			sRendererAPI->ClearStencil();
		}

		static void StencilMask(uint32_t mask)
		{
			sRendererAPI->StencilMask(mask);
		}

		//cull
		static void Cull(int32_t Bit)
		{
			sRendererAPI->Cull(Bit);
		}

		static void CullFrontOrBack(int32_t Bit)
		{
			sRendererAPI->CullFrontOrBack(Bit);
		}
		
		// framebuffer
		[[nodiscard]] static int GetDrawFrameBuffer()
		{
			return sRendererAPI->GetDrawFrameBuffer();
		}

		static void BindFrameBuffer(uint32_t framebufferID)
		{
			sRendererAPI->BindFrameBuffer(framebufferID);
		}

    private:
        static Scope<RendererAPI> sRendererAPI;
    };
}
