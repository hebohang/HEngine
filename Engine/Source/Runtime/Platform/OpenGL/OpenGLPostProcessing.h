#pragma once

#include "Runtime/Renderer/PostProcessing.h"

namespace HEngine
{
    class OpenGLPostProcessing : public PostProcessing
    {
    public:
        OpenGLPostProcessing(const PostProcessingType& type) : PostProcessing(type) {}
    public:
        virtual uint32_t ExcuteAndReturnFinalTex(const Ref<Framebuffer>& fb) override;
    private:
        uint32_t DoMSAA(const Ref<Framebuffer>& fb);
        uint32_t DoOutline(const Ref<Framebuffer>& fb);
        uint32_t DoCartoon(const Ref<Framebuffer>& fb);
    };
}