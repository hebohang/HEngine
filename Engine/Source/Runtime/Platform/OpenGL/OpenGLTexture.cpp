#include "hepch.h"

#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Platform/OpenGL/OpenGLTexture.h"

#include <stb_image.h>

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
        stbi_uc* data =  stbi_load(AssetManager::GetInstance().GetFullPath(path).string().c_str(), &width, &height, &channels, 0);
        
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

    void OpenGLTexture2D::UnBind() const
    {
        glBindTexture(GL_TEXTURE, 0);
    }

    // refer to https://learnopengl-cn.github.io/04%20Advanced%20OpenGL/06%20Cubemaps/
    OpenGLCubeMapTexture::OpenGLCubeMapTexture(std::vector<std::string>& paths)
        : mPaths(paths)
    {
        glDeleteTextures(1, &mRendererID);
        glGenTextures(1, &mRendererID);
        glBindTexture(GL_TEXTURE_CUBE_MAP, mRendererID);

        int width, height, nrChannels;
        for (unsigned int i = 0; i < paths.size(); i++)
        {
            unsigned char* data = stbi_load(paths[i].c_str(), &width, &height, &nrChannels, 0);
            if (data)
            {
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i,
                    0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data
                );
                stbi_image_free(data);
            }
            else
            {
                HE_CORE_ERROR("Cubemap don't loaded correctly!");
                stbi_image_free(data);
            }
        }

        mWidth = width;
        mHeight = height;

        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    }

    OpenGLCubeMapTexture::~OpenGLCubeMapTexture()
    {
        glDeleteTextures(1, &mRendererID);
    }

    void OpenGLCubeMapTexture::Bind(uint32_t slot) const
    {
        glActiveTexture(GL_TEXTURE0 + slot);
        glBindTexture(GL_TEXTURE_CUBE_MAP, mRendererID);
    }

    void OpenGLCubeMapTexture::UnBind() const
    {
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    }
}
