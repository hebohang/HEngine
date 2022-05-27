#pragma once

#include "Runtime/Renderer/Framebuffer.h"
#include "Runtime/Renderer/VertexArray.h"
#include "Runtime/Renderer/Texture.h"

namespace HEngine
{
    enum class PostProcessingType
    {
        None = 0,
        MSAA = 1,
        Outline,
        Cartoon,
        GrayScale,
        GaussianBlur,
        FxaaConsole,
    };

    class PostProcessing
    {
    public:
        PostProcessing(const PostProcessingType& type);
        virtual ~PostProcessing() {}
    public:
        static void Init();
        virtual uint32_t ExcuteAndReturnFinalTex(const Ref<Framebuffer>& fb) { return 0; };
    protected:
        void DoPostProcessing();
    public:
        PostProcessingType mType;
        static Ref<Framebuffer> mFramebuffer;
        static Ref<Texture2D> mIntermediateScreenTex;
    protected:
        static Ref<VertexArray> mScreenQuadVAO;
        static Ref<VertexBuffer> mScreenQuadVBO;
        static Ref<IndexBuffer> mScreenQuadIBO;
    };
}