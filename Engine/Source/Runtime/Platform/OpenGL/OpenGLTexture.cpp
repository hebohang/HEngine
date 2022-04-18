#include "hepch.h"
#include "OpenGLTexture.h"

#include "stb_image.h"

#include <glad/glad.h>

namespace HEngine
{
    OpenGLTexture2D::OpenGLTexture2D(uint32_t width, uint32_t height)
        : mWidth(width), mHeight(height)
    {
        mInternalFormat = GL_RGBA8;
        mDataFormat = GL_RGBA;
        
        glCreateTextures(GL_TEXTURE_2D, 1, &mRendererID);
        glTextureStorage2D(mRendererID, 1, mInternalFormat, mWidth, mHeight);

        glTextureParameteri(mRendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(mRendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glTextureParameteri(mRendererID, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(mRendererID, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    OpenGLTexture2D::OpenGLTexture2D(const std::string& path)
        : mPath(path)
    {
        int width, height, channels;
        stbi_set_flip_vertically_on_load(1);
        stbi_uc* data =  stbi_load(path.c_str(), &width, &height, &channels, 0);
        
		if (data)
		{
			mIsLoaded = true;

			mWidth = width;
			mHeight = height;

			GLenum internalFormat = 0, dataFormat = 0;
			if (channels == 4)
			{
				internalFormat = GL_RGBA8;
				dataFormat = GL_RGBA;
			}
			else if (channels == 3)
			{
				internalFormat = GL_RGB8;
				dataFormat = GL_RGB;
			}

			mInternalFormat = internalFormat;
			mDataFormat = dataFormat;

			HE_CORE_ASSERT(internalFormat & dataFormat, "Format not supported!");

			glCreateTextures(GL_TEXTURE_2D, 1, &mRendererID);
			glTextureStorage2D(mRendererID, 1, internalFormat, mWidth, mHeight);

			glTextureParameteri(mRendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTextureParameteri(mRendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

			glTextureParameteri(mRendererID, GL_TEXTURE_WRAP_S, GL_REPEAT);
			glTextureParameteri(mRendererID, GL_TEXTURE_WRAP_T, GL_REPEAT);

			glTextureSubImage2D(mRendererID, 0, 0, 0, mWidth, mHeight, dataFormat, GL_UNSIGNED_BYTE, data);

			stbi_image_free(data);
		}
    }

    OpenGLTexture2D::~OpenGLTexture2D()
    {
        glDeleteTextures(1, &mRendererID);
    }

    void OpenGLTexture2D::SetData(void* data, uint32_t size)
    {
        uint32_t bpp = mDataFormat == GL_RGBA ? 4 : 3;  // bytes per pixel
        HE_CORE_ASSERT(size == mWidth * mHeight * bpp, "Data must be entire texture!");
        glTextureSubImage2D(mRendererID, 0, 0, 0, mWidth, mHeight, mDataFormat, GL_UNSIGNED_BYTE, data);
    }

    void OpenGLTexture2D::Bind(uint32_t slot) const
    {
        glBindTextureUnit(slot, mRendererID);
    }
}
