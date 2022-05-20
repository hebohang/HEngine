#include "hepch.h"

#include "Runtime/Platform/OpenGL/OpenGLPostProcessing.h"
#include "Runtime/Library/ShaderLibrary.h"

#include <glad/glad.h>

namespace HEngine
{
    uint32_t OpenGLPostProcessing::DoMSAA(const Ref<Framebuffer>& fb)
    {
        uint32_t width = fb->GetSpecification().Width;
        uint32_t height = fb->GetSpecification().Height;
        mFramebuffer->Bind();
        mFramebuffer->Resize(width, height);

        fb->BindReadFramebuffer();
        mFramebuffer->BindDrawFramebuffer();
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

        return mFramebuffer->GetColorAttachmentRendererID();
    }

    uint32_t OpenGLPostProcessing::DoOutline(const Ref<Framebuffer>& fb)
    {
        Library<Shader>::GetInstance().Get("Post_Outline")->Bind();
        Library<Shader>::GetInstance().Get("Post_Outline")->SetInt("screenTexture", 0);
        fb->Bind();
        DoPostProcessing();
        return fb->GetColorAttachmentRendererID();
    }

    uint32_t OpenGLPostProcessing::ExcuteAndReturnFinalTex(const Ref<Framebuffer>& fb)
    {
        switch (mType)
        {
        case PostProcessingType::None:
            return 0;
            break;
        case PostProcessingType::MSAA:
            return DoMSAA(fb);
            break;
        case PostProcessingType::Outline:
            return DoOutline(fb);
            break;
        default:
            return 0;
            break;
        }

        return 0;
    }
}