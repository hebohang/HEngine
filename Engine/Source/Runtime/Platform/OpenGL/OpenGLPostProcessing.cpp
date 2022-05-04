#include "hepch.h"

#include "Runtime/Platform/OpenGL/OpenGLPostProcessing.h"

#include <glad/glad.h>

namespace HEngine
{
    static uint32_t DoMSAA(const Ref<Framebuffer>& fb)
    {
        static bool bInit = true;

        static unsigned int intermediateFBO;
        static unsigned int screenTexture;
        static unsigned int tempTex;

        if (bInit)
        {
            glGenFramebuffers(1, &intermediateFBO);
            glGenTextures(1, &screenTexture);
            glGenRenderbuffers(1, &tempTex);
            bInit = false;
        }
        uint32_t width = fb->GetSpecification().Width;
        uint32_t height = fb->GetSpecification().Height;
        glBindFramebuffer(GL_FRAMEBUFFER, intermediateFBO);

        // create a color attachment texture
        glBindTexture(GL_TEXTURE_2D, screenTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, screenTexture, 0);	// we only need a color buffer

        //unsigned int tempTex;
        //glGenTextures(1, &tempTex);
        //glBindTexture(GL_TEXTURE_2D, tempTex);
        //glTexImage2D(GL_TEXTURE_2D, 0, GL_R32I, width, height, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, NULL);
        //glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        //glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        //glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, tempTex, 0);	// we only need a color buffer

        glBindRenderbuffer(GL_RENDERBUFFER, tempTex);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_R32I, width, height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 1, GL_RENDERBUFFER, tempTex);

        fb->BindReadFramebuffer();
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, intermediateFBO);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

        //glDrawBuffer(intermediateFBO);
        //glReadBuffer(intermediateFBO);
        //glBindTexture(GL_TEXTURE_2D, screenTexture);

        return screenTexture;
    }

    uint32_t OpenGLPostProcessing::ExcuteAndReturnFinalTex(const Ref<Framebuffer>& fb)
    {
        switch (mType)
        {
        case HEngine::PostProcessingType::None:
            return 0;
            break;
        case HEngine::PostProcessingType::MSAA:
            return DoMSAA(fb);
            break;
        default:
            return 0;
            break;
        }

        return 0;
    }
}