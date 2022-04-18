#pragma once

#include "Runtime/Renderer/VertexArray.h"

namespace HEngine
{
    class OpenGLVertexArray : public VertexArray
    {
    public:
        OpenGLVertexArray();
        virtual ~OpenGLVertexArray();

        virtual void Bind() const override;
        virtual void Unbind() const override;

        virtual void AddVertexBuffer(const Ref<VertexBuffer>& vertexBuffer) override;
        virtual void SetIndexBuffer(const Ref<IndexBuffer>& indexBuffer) override;

        virtual const std::vector<Ref<VertexBuffer>>& GetVertexBuffers() const override { return mVertexBuffers; }
        virtual const Ref<IndexBuffer>& GetIndexBuffer() const override { return mIndexBuffer; }
    private:
        uint32_t mRendererID;
		uint32_t mVertexBufferIndex = 0;
        std::vector<Ref<VertexBuffer>> mVertexBuffers;
        Ref<IndexBuffer> mIndexBuffer;
    };
}
