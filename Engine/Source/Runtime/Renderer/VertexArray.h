#pragma once

#include <memory>
#include "Runtime/Renderer/Buffer.h"

namespace HEngine
{
    class VertexArray
    {
    public:
        virtual ~VertexArray() {}

        virtual void Bind() const = 0;
        virtual void Unbind() const = 0;

        virtual void AddVertexBuffer(const Ref<VertexBuffer>& vertexBuffer) = 0;
        virtual void SetIndexBuffer(const Ref<IndexBuffer>& indexBuffer) = 0;

        [[nodiscard]] virtual const std::vector<Ref<VertexBuffer>>& GetVertexBuffers() const = 0;
        [[nodiscard]] virtual const Ref<IndexBuffer>& GetIndexBuffer() const = 0;

        static Ref<VertexArray> Create();
    };
}