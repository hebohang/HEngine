#include "hepch.h"

#include "Runtime/Platform/OpenGL/OpenGLVertexBuffer.h"

#include <glad/glad.h>

namespace HEngine
{
    static GLenum OpenGLUsage(VertexBufferUsage usage)
    {
        switch (usage)
        {
        case VertexBufferUsage::Static:    return GL_STATIC_DRAW;
        case VertexBufferUsage::Dynamic:   return GL_DYNAMIC_DRAW;
        }
        HE_CORE_ASSERT(false, "Unknown vertex buffer usage");
        return 0;
    }

    OpenGLVertexBuffer::OpenGLVertexBuffer(uint32_t size, VertexBufferUsage usage)
        : mUsage(usage)
    {
        glCreateBuffers(1, &mRendererID);
        glBindBuffer(GL_ARRAY_BUFFER, mRendererID);
        glBufferData(GL_ARRAY_BUFFER, size, nullptr, OpenGLUsage(mUsage));
    }

    OpenGLVertexBuffer::OpenGLVertexBuffer(void* vertices, uint32_t size, VertexBufferUsage usage)
        : mUsage(usage)
    {
        glCreateBuffers(1, &mRendererID);
        glBindBuffer(GL_ARRAY_BUFFER, mRendererID);
        glBufferData(GL_ARRAY_BUFFER, size, vertices, OpenGLUsage(mUsage));
    }

    OpenGLVertexBuffer::~OpenGLVertexBuffer()
    {
        glDeleteBuffers(1, &mRendererID);
    }

    void OpenGLVertexBuffer::Bind() const
    {
        glBindBuffer(GL_ARRAY_BUFFER, mRendererID);
    }

    void OpenGLVertexBuffer::Unbind() const
    {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    void OpenGLVertexBuffer::SetData(const void* data, uint32_t size)
    {
        glBindBuffer(GL_ARRAY_BUFFER, mRendererID);
        glBufferSubData(GL_ARRAY_BUFFER, 0, size, data);
    }
}