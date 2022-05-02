#pragma once

#include "Runtime/Renderer/Framebuffer.h"

namespace HEngine
{
    enum class PostProcessingType
    {
        None = 1,
        MSAA,

    };

    class PostProcessing
    {
    public:
        PostProcessing(const PostProcessingType& type) : mType{ type } {};
        PostProcessing(const PostProcessing&) = default;
        virtual ~PostProcessing() {}
    public:
        virtual uint32_t ExcuteAndReturnFinalTex(const Ref<Framebuffer>& fb) { return 0; };
    protected:
        PostProcessingType mType;
    };
}