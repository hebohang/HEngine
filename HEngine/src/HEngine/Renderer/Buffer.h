#pragma once

#include <vector>

namespace HEngine
{
    enum class ShaderDataType
    {
        None = 0, Float, Float2, Float3, Float4, Mat3, Mat4, Int, Int2, Int3, Int4, Bool
    };

    static uint32_t ShaderDataTypeSize(ShaderDataType type)
    {
        switch (type)
        {
        case HEngine::ShaderDataType::Float:    return 4;
        case HEngine::ShaderDataType::Float2:   return 4 * 2;
        case HEngine::ShaderDataType::Float3:   return 4 * 3;
        case HEngine::ShaderDataType::Float4:   return 4 * 4;
        case HEngine::ShaderDataType::Mat3:     return 4 * 3 * 3;
        case HEngine::ShaderDataType::Mat4:     return 4 * 4 * 4;
        case HEngine::ShaderDataType::Int:      return 4;
        case HEngine::ShaderDataType::Int2:     return 4 * 2;
        case HEngine::ShaderDataType::Int3:     return 4 * 3;
        case HEngine::ShaderDataType::Int4:     return 4 * 4;
        case HEngine::ShaderDataType::Bool:     return 1;
        }

        HE_CORE_ASSERT(false, "Unknown ShaderDataType!");
        return 0;
    }

    struct BufferElement
    {
        std::string Name;
        ShaderDataType Type;
        uint32_t Size;
        uint32_t Offset;
        bool Normalized;

        BufferElement() = default;

        BufferElement(ShaderDataType type, const std::string& name, bool normalized = false)
            : Name(name), Type(type), Size(ShaderDataTypeSize(type)), Offset(0), Normalized(normalized)
        {
        }

        uint32_t GetComponentCount() const
        {
            switch (Type)
            {
            case HEngine::ShaderDataType::Float:    return 1;
            case HEngine::ShaderDataType::Float2:   return 2;
            case HEngine::ShaderDataType::Float3:   return 3;
            case HEngine::ShaderDataType::Float4:   return 4;
            case HEngine::ShaderDataType::Mat3:     return 3 * 3;
            case HEngine::ShaderDataType::Mat4:     return 4 * 4;
            case HEngine::ShaderDataType::Int:      return 1;
            case HEngine::ShaderDataType::Int2:     return 2;
            case HEngine::ShaderDataType::Int3:     return 3;
            case HEngine::ShaderDataType::Int4:     return 4;
            case HEngine::ShaderDataType::Bool:     return 1;
            }

            HE_CORE_ASSERT(false, "Unknown ShaderDataType!");
            return 0;
        }
    };

    class BufferLayout
    {
    public:
        BufferLayout() = default;

        BufferLayout(const std::initializer_list<BufferElement>& elements) 
            : m_Elements(elements)
        {
            CalculateOffsetsAndStride();
        }

        inline uint32_t GetStride() const { return m_Stride; }
        inline const std::vector<BufferElement>& GetElements() const { return m_Elements; }

        std::vector<BufferElement>::iterator begin() { return m_Elements.begin(); }
        std::vector<BufferElement>::iterator end() { return m_Elements.end(); }        
        std::vector<BufferElement>::const_iterator begin() const { return m_Elements.begin(); }
        std::vector<BufferElement>::const_iterator end() const { return m_Elements.end(); }
    private:
        void CalculateOffsetsAndStride()
        {
            uint32_t offset = 0;
            m_Stride = 0;
            for (auto& element : m_Elements)
            {
                element.Offset = offset;
                offset += element.Size;
                m_Stride += element.Size;
            }
        }
    private:
        std::vector<BufferElement> m_Elements;
        uint32_t m_Stride = 0;
    };

    class VertexBuffer
    {
    public:
        virtual ~VertexBuffer() = default;

        virtual void Bind() const = 0;
        virtual void Unbind() const = 0;

        virtual void SetData(const void* data, uint32_t size) = 0;

        virtual const BufferLayout& GetLayout() const = 0;
        virtual void SetLayout(const BufferLayout& layout) = 0;

        static Ref<VertexBuffer> Create(uint32_t size);
        static Ref<VertexBuffer> Create(float* vertices, uint32_t size);
    };

    // Currently HEngine only supports 32-bit index buffers
    class IndexBuffer
    {
    public:
        virtual ~IndexBuffer() = default;

        virtual void Bind() const = 0;
        virtual void Unbind() const = 0;

        virtual uint32_t GetCount() const = 0;

        static Ref<IndexBuffer> Create(uint32_t* indices, uint32_t size);
    };
}