#pragma once

#include "Runtime/Renderer/VertexBuffer.h"

namespace HEngine
{
    class OpenGLVertexBuffer : public VertexBuffer
    {
    public:
        OpenGLVertexBuffer(uint32_t size, VertexBufferUsage usage = VertexBufferUsage::Dynamic);
        OpenGLVertexBuffer(void* vertices, uint32_t size, VertexBufferUsage usage = VertexBufferUsage::Static);
        virtual ~OpenGLVertexBuffer();

        virtual void Bind() const override;
        virtual void Unbind() const override;

        virtual void SetData(const void* data, uint32_t size) override;

        virtual const BufferLayout& GetLayout() const override { return mLayout; }
        virtual void SetLayout(const BufferLayout& layout) override { mLayout = layout; }
    private:
        uint32_t mRendererID;
        VertexBufferUsage mUsage;
        BufferLayout mLayout;
    };
}