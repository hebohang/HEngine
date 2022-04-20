#pragma once

namespace HEngine
{
    // Currently HEngine only supports 32-bit index buffers
    class IndexBuffer
    {
    public:
        virtual ~IndexBuffer() = default;

        virtual void Bind() const = 0;
        virtual void Unbind() const = 0;

        virtual void SetData(const void* data, uint32_t count) = 0;

        [[nodiscard]] virtual uint32_t GetCount() const = 0;

        static Ref<IndexBuffer> Create(uint32_t count);
        static Ref<IndexBuffer> Create(uint32_t* indices, uint32_t count);
    };
}