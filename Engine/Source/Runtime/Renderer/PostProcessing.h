#pragma once

#include "Runtime/Renderer/Framebuffer.h"
#include "Runtime/Renderer/VertexArray.h"
#include "Runtime/Renderer/Texture.h"

namespace HEngine
{
    enum class PostProcessingType
    {
        None = 1,
        MSAA,
        Outline,
        Cartoon,
    };

    class PostProcessing
    {
        friend class RenderPass;
    public:
        PostProcessing(const PostProcessingType& type);
        virtual ~PostProcessing() {}
    public:
        static void Init();
        virtual uint32_t ExcuteAndReturnFinalTex(const Ref<Framebuffer>& fb) { return 0; };
    protected:
        void DoPostProcessing();
    protected:
        PostProcessingType mType;
        static Ref<VertexArray> mScreenQuadVAO;
        static Ref<VertexBuffer> mScreenQuadVBO;
        static Ref<IndexBuffer> mScreenQuadIBO;
        static Ref<Framebuffer> mFramebuffer;
    };
}