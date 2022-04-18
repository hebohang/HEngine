#include "hepch.h"
#include "OpenGLBuffer.h"

#include <glad/glad.h>

namespace HEngine
{
    ///////////////////////////////////////////////////////////////////
    // VertexBuffer ///////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////

    OpenGLVertexBuffer::OpenGLVertexBuffer(uint32_t size)
    {
        glCreateBuffers(1, &mRendererID);
        glBindBuffer(GL_ARRAY_BUFFER, mRendererID);
        glBufferData(GL_ARRAY_BUFFER, size, nullptr, GL_DYNAMIC_DRAW);
    }

    OpenGLVertexBuffer::OpenGLVertexBuffer(float* vertices, uint32_t size)
    {
        glCreateBuffers(1, &mRendererID);
        glBindBuffer(GL_ARRAY_BUFFER, mRendererID);
        glBufferData(GL_ARRAY_BUFFER, size, vertices, GL_STATIC_DRAW);
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

    //////////////////////////////////////////////////////////////////
    // IndexBuffer ///////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////

    OpenGLIndexBuffer::OpenGLIndexBuffer(uint32_t* indices, uint32_t count)
        : mCount(count)
    {
        glCreateBuffers(1, &mRendererID);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mRendererID);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, count * sizeof(uint32_t), indices, GL_STATIC_DRAW);
    }

    OpenGLIndexBuffer::~OpenGLIndexBuffer()
    {
        glDeleteBuffers(1, &mRendererID);
    }

    void OpenGLIndexBuffer::Bind() const
    {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mRendererID);
    }

    void OpenGLIndexBuffer::Unbind() const
    {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
}