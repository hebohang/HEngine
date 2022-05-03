#include "hepch.h"

#include "Runtime/Platform/OpenGL/OpenGLFramebuffer.h"

#include <glad/glad.h>

namespace HEngine
{
    static const uint32_t s_MaxFramebufferSize = 8192;

	namespace Utils
	{
		static GLenum TextureTarget(bool multisampled)
		{
			return multisampled ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
		}

		static void AttachColorTexture(uint32_t& id, int samples, GLenum internalFormat, GLenum format, uint32_t width, uint32_t height, int index)
		{
			glGenTextures(1, &id);
			bool multisampled = samples > 1;
			if (multisampled)
			{
				glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, id);
				glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, internalFormat, width, height, GL_TRUE);
				glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
			}
			else
			{
				glBindTexture(GL_TEXTURE_2D, id);
				glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, nullptr);

				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

				glBindTexture(GL_TEXTURE_2D, 0);
			}

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + index, TextureTarget(multisampled), id, 0);
		}

		static void AttachColorRenderBuffer(uint32_t id, int samples, GLenum internalFormat, uint32_t width, uint32_t height, int index)
		{
			glGenRenderbuffers(1, &id);
			bool multisampled = samples > 1;
			if (multisampled)
			{
				glBindRenderbuffer(GL_RENDERBUFFER, id);
				glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, internalFormat, width, height);
				glBindRenderbuffer(GL_RENDERBUFFER, 0);
			}
			else
			{
				glBindRenderbuffer(GL_RENDERBUFFER, id);
				glRenderbufferStorage(GL_RENDERBUFFER, internalFormat, width, height);
				glBindRenderbuffer(GL_RENDERBUFFER, 0);
			}

			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + index, GL_RENDERBUFFER, id);
		}

		static void AttachDepthTexture(uint32_t& id, int samples, GLenum format, GLenum attachmentType, uint32_t width, uint32_t height)
		{
			glGenTextures(1, &id);
			bool multisampled = samples > 1;
			if (multisampled)
			{
				glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, id);
				glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, format, width, height, GL_FALSE);
				glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
			}
			else
			{
				glBindTexture(GL_TEXTURE_2D, id);
				glTexStorage2D(GL_TEXTURE_2D, 1, format, width, height);

				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

				glBindTexture(GL_TEXTURE_2D, 0);
			}

			glFramebufferTexture2D(GL_FRAMEBUFFER, attachmentType, TextureTarget(multisampled), id, 0);
		}

		static void AttachDepthRenderBuffer(uint32_t id, int samples, GLenum format, GLenum attachmentType, uint32_t width, uint32_t height)
		{
			glGenRenderbuffers(1, &id);
			bool multisampled = samples > 1;
			if (multisampled)
			{
				glBindRenderbuffer(GL_RENDERBUFFER, id);
				glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, width, height);
				glBindRenderbuffer(GL_RENDERBUFFER, 0);
			}
			else
			{
				glBindRenderbuffer(GL_RENDERBUFFER, id);
				glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
				glBindRenderbuffer(GL_RENDERBUFFER, 0);
			}

			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, id);
		}

		static bool IsDepthFormat(FramebufferTextureFormat format)
		{
			switch (format)
			{
			case HEngine::FramebufferTextureFormat::DEPTH24STENCIL8:
				return true;
				break;
			}

			return false;
		}
	}

    OpenGLFramebuffer::OpenGLFramebuffer(const FramebufferSpecification& spec)
        : mSpecification(spec)
    {
		for (auto spec : mSpecification.Attachments.Attachments)
		{
			if (!Utils::IsDepthFormat(spec.TextureFormat))
				mColorAttachmentSpecifications.emplace_back(spec);
			else
				mDepthAttachmentSpecification = spec;
		}

        Invalidate();
    }

    OpenGLFramebuffer::~OpenGLFramebuffer()
    {
        glDeleteFramebuffers(1, &mRendererID);
        glDeleteTextures(mColorAttachments.size(), mColorAttachments.data());
        glDeleteTextures(1, &mDepthAttachment);
    }

    void OpenGLFramebuffer::Invalidate()
    {
        if (mRendererID)
        {
            glDeleteFramebuffers(1, &mRendererID);
            glDeleteTextures(mColorAttachments.size(), mColorAttachments.data());
            glDeleteTextures(1, &mDepthAttachment);

			mColorAttachments.clear();
			mDepthAttachment = 0;
        }

        glCreateFramebuffers(1, &mRendererID);
        glBindFramebuffer(GL_FRAMEBUFFER, mRendererID);

		bool multisample = mSpecification.Samples > 1;

		// Attachments
		if (mColorAttachmentSpecifications.size())
		{
			mColorAttachments.resize(mColorAttachmentSpecifications.size());

			for (size_t i = 0; i < mColorAttachments.size(); i++)
			{
				switch (mColorAttachmentSpecifications[i].TextureFormat)
				{
				case FramebufferTextureFormat::RGBA8:
					Utils::AttachColorTexture(mColorAttachments[i], mSpecification.Samples, GL_RGBA8, GL_RGBA, mSpecification.Width, mSpecification.Height, i);
					break;
				case FramebufferTextureFormat::RED_INTEGER:
					//Utils::AttachColorTexture(mColorAttachments[i], mSpecification.Samples, GL_R32I, GL_RED_INTEGER, mSpecification.Width, mSpecification.Height, i);
					Utils::AttachColorRenderBuffer(mColorAttachments[i], mSpecification.Samples, GL_R32I, mSpecification.Width, mSpecification.Height, i);  // We dont use multisample in Renderbuffer
					break;
				}
			}
		}

		if (mDepthAttachmentSpecification.TextureFormat != FramebufferTextureFormat::None)
		{
			switch (mDepthAttachmentSpecification.TextureFormat)
			{
			case FramebufferTextureFormat::DEPTH24STENCIL8:
				Utils::AttachDepthRenderBuffer(mDepthAttachment, mSpecification.Samples, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL_ATTACHMENT, mSpecification.Width, mSpecification.Height);
				break;
			}
		}

		if (mColorAttachments.size() > 1)
		{
			HE_CORE_ASSERT(mColorAttachments.size() <= 4);
			GLenum buffers[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
			glDrawBuffers(mColorAttachments.size(), buffers);
		}
		else if (mColorAttachments.empty())
		{
			// Only depth-pass
			glDrawBuffer(GL_NONE);
		}

		HE_CORE_ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Framebuffer is incomplete!");

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void OpenGLFramebuffer::Bind()
    {
        glBindFramebuffer(GL_FRAMEBUFFER, mRendererID);
        glViewport(0, 0, mSpecification.Width, mSpecification.Height);
    }

    void OpenGLFramebuffer::Unbind()
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

	void OpenGLFramebuffer::BindReadFramebuffer()
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, mRendererID);
	}

	void OpenGLFramebuffer::BindDrawFramebuffer()
	{
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mRendererID);
	}

    void OpenGLFramebuffer::Resize(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0 || width > s_MaxFramebufferSize || height > s_MaxFramebufferSize)
        {
            HE_CORE_WARN("Attempted to rezize framebuffer to {0}, {1}", width, height);
            return;
        }
        mSpecification.Width = width;
        mSpecification.Height = height;

        Invalidate();
    }

	int OpenGLFramebuffer::ReadPixel(uint32_t attachmentIndex, int x, int y)
	{
		HE_CORE_ASSERT(attachmentIndex < mColorAttachments.size());

		uint32_t width = mSpecification.Width;
		uint32_t height = mSpecification.Height;
		unsigned int intermediateFBO;
		glGenFramebuffers(1, &intermediateFBO);
		glBindFramebuffer(GL_FRAMEBUFFER, intermediateFBO);

		unsigned int tempTex;
		glGenRenderbuffers(1, &tempTex);
		glBindRenderbuffer(GL_RENDERBUFFER, tempTex);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_R32I, width, height);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_RENDERBUFFER, tempTex);

		glBindFramebuffer(GL_FRAMEBUFFER, mRendererID);
		glReadBuffer(GL_COLOR_ATTACHMENT1);

		glBindFramebuffer(GL_FRAMEBUFFER, intermediateFBO);
		glDrawBuffer(GL_COLOR_ATTACHMENT1);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, mRendererID);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, intermediateFBO);
		glNamedFramebufferReadBuffer(mRendererID, GL_COLOR_ATTACHMENT1);
		glNamedFramebufferDrawBuffer(intermediateFBO, GL_COLOR_ATTACHMENT1);
		glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

		glBindFramebuffer(GL_FRAMEBUFFER, mRendererID);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		//glDrawBuffer(GL_COLOR_ATTACHMENT0);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, intermediateFBO);
		//glNamedFramebufferReadBuffer(intermediateFBO, GL_COLOR_ATTACHMENT1);
		glReadBuffer(GL_COLOR_ATTACHMENT0 + attachmentIndex);
		int pixelData;
		glReadPixels(x, y, 1, 1, GL_RED_INTEGER, GL_INT, &pixelData);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		//glDrawBuffer(GL_COLOR_ATTACHMENT0);
		return pixelData;
	}

	void OpenGLFramebuffer::ClearAttachment(uint32_t attachmentIndex, int value)
	{
		HE_CORE_ASSERT(attachmentIndex < mColorAttachments.size());

		glBindFramebuffer(GL_FRAMEBUFFER, mRendererID);
		auto& spec = mColorAttachmentSpecifications[attachmentIndex];

		switch (spec.TextureFormat)
		{
		case FramebufferTextureFormat::RED_INTEGER:
			//glClearTexImage(mColorAttachments[attachmentIndex], 0, GL_R32I, GL_INT, &value);
			glClearBufferiv(GL_COLOR, attachmentIndex, &value);
			break;
		case FramebufferTextureFormat::RGBA8:
			glClearTexImage(mColorAttachments[attachmentIndex], 0, GL_RGBA8, GL_INT, &value);
			break;
		case FramebufferTextureFormat::DEPTH24STENCIL8:
			glClearBufferiv(GL_DEPTH24_STENCIL8, attachmentIndex, &value);
		}
	}

	uint32_t OpenGLFramebuffer::GetColorAttachmentRendererID(uint32_t index) const
	{
		HE_CORE_ASSERT(index < mColorAttachments.size()); return mColorAttachments[index];

		//configure second post-processing framebuffer
		unsigned int intermediateFBO;
		glGenFramebuffers(1, &intermediateFBO);
		glBindFramebuffer(GL_FRAMEBUFFER, intermediateFBO);
		// create a color attachment texture
		unsigned int screenTexture;
		glGenTextures(1, &screenTexture);
		glBindTexture(GL_TEXTURE_2D, screenTexture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, mSpecification.Width, mSpecification.Height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, screenTexture, 0);	// we only need a color buffer

		glBindFramebuffer(GL_READ_FRAMEBUFFER, mRendererID);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, intermediateFBO);
		glBlitFramebuffer(0, 0, mSpecification.Width, mSpecification.Height, 0, 0, mSpecification.Width, mSpecification.Height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		//glBindTexture(GL_TEXTURE_2D, 0);

		//return mColorAttachments[index];
		return screenTexture;
	}
}
