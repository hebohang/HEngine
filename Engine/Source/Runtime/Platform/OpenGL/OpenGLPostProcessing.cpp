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

        fb->BindReadFramebuffer();
        mFramebuffer->BindDrawFramebuffer();
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        return mFramebuffer->GetColorAttachmentRendererID();
    }

    uint32_t OpenGLPostProcessing::DoPostWithShader(const Ref<Framebuffer>& fb, const Ref<Shader>& shader)
    {
        uint32_t width = fb->GetSpecification().Width;
        uint32_t height = fb->GetSpecification().Height;
        mFramebuffer->Bind();

        // copy the framebuffer to the intermediate screen texture
        mFramebuffer->BindReadFramebuffer();
        mIntermediateScreenTex->Bind();
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

        mFramebuffer->BindDrawFramebuffer();
        shader->Bind();
        shader->SetInt("screenTexture", 0);
        DoPostProcessing();

        return mFramebuffer->GetColorAttachmentRendererID();
    }

    uint32_t OpenGLPostProcessing::ExcuteAndReturnFinalTex(const Ref<Framebuffer>& fb)
    {
        uint32_t re = 0;

        switch (mType)
        {
        case PostProcessingType::None:
            return 0;
            break;
        case PostProcessingType::MSAA:
            re = DoMSAA(fb);
            break;
        case PostProcessingType::Outline:
            re = DoPostWithShader(fb, Library<Shader>::GetInstance().Get("Post_Outline"));
            break;
        case PostProcessingType::Cartoon:
            re = DoPostWithShader(fb, Library<Shader>::GetInstance().Get("Post_Cartoon"));
            break;
        case PostProcessingType::GrayScale:
            re = DoPostWithShader(fb, Library<Shader>::GetInstance().Get("Post_GrayScale"));
            break;
        case PostProcessingType::GaussianBlur:
            re = DoPostWithShader(fb, Library<Shader>::GetInstance().Get("Post_GaussianBlur"));
            break;
        default:
            return 0;
            break;
        }

        // it seems imgui need it ??
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

        return re;
    }
}