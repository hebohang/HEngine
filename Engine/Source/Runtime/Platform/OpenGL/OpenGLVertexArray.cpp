#include "hepch.h"
#include "OpenGLVertexArray.h"

#include <glad/glad.h>

namespace HEngine
{
    static GLenum ShaderDataTypeToOpenGLBaseType(ShaderDataType type)
    {
        switch (type)
        {
        case HEngine::ShaderDataType::Float:     return GL_FLOAT;
        case HEngine::ShaderDataType::Float2:	 return GL_FLOAT;
        case HEngine::ShaderDataType::Float3:	 return GL_FLOAT;
        case HEngine::ShaderDataType::Float4:	 return GL_FLOAT;
        case HEngine::ShaderDataType::Mat3:		 return GL_FLOAT;
        case HEngine::ShaderDataType::Mat4:		 return GL_FLOAT;
        case HEngine::ShaderDataType::Int:		 return GL_INT;
        case HEngine::ShaderDataType::Int2:		 return GL_INT;
        case HEngine::ShaderDataType::Int3:		 return GL_INT;
        case HEngine::ShaderDataType::Int4:		 return GL_INT;
        case HEngine::ShaderDataType::Bool:		 return GL_BOOL;
        }

        HE_CORE_ASSERT(false, "Unknown ShaderDataType!");
        return 0;
    }

    OpenGLVertexArray::OpenGLVertexArray()
    {
        glCreateVertexArrays(1, &mRendererID);
    }

    OpenGLVertexArray::~OpenGLVertexArray()
    {
        glDeleteVertexArrays(1, &mRendererID);
    }

    void OpenGLVertexArray::Bind() const
    {
        glBindVertexArray(mRendererID);
    }

    void OpenGLVertexArray::Unbind() const
    {
        glBindVertexArray(0);
    }

    void OpenGLVertexArray::AddVertexBuffer(const Ref<VertexBuffer>& vertexBuffer)
    {
        HE_CORE_ASSERT(vertexBuffer->GetLayout().GetElements().size(), "Vertex buffer has no layout!");

        glBindVertexArray(mRendererID);
        vertexBuffer->Bind();

        const auto& layout = vertexBuffer->GetLayout();
        for (const auto& element : layout)
        {
			switch (element.Type)
			{
			case ShaderDataType::Float:
			case ShaderDataType::Float2:
			case ShaderDataType::Float3:
			case ShaderDataType::Float4:
			{
				glEnableVertexAttribArray(mVertexBufferIndex);
				glVertexAttribPointer(mVertexBufferIndex,
					element.GetComponentCount(),
					ShaderDataTypeToOpenGLBaseType(element.Type),
					element.Normalized ? GL_TRUE : GL_FALSE,
					layout.GetStride(),
					(const void*)element.Offset);
				mVertexBufferIndex++;
				break;
			}
			case ShaderDataType::Int:
			case ShaderDataType::Int2:
			case ShaderDataType::Int3:
			case ShaderDataType::Int4:
			case ShaderDataType::Bool:
			{
				glEnableVertexAttribArray(mVertexBufferIndex);
				glVertexAttribIPointer(mVertexBufferIndex,
					element.GetComponentCount(),
					ShaderDataTypeToOpenGLBaseType(element.Type),
					layout.GetStride(),
					(const void*)element.Offset);
				mVertexBufferIndex++;
				break;
			}
			case ShaderDataType::Mat3:
			case ShaderDataType::Mat4:
			{
				uint8_t count = element.GetComponentCount();
				for (uint8_t i = 0; i < count; i++)
				{
					glEnableVertexAttribArray(mVertexBufferIndex);
					glVertexAttribPointer(mVertexBufferIndex,
						count,
						ShaderDataTypeToOpenGLBaseType(element.Type),
						element.Normalized ? GL_TRUE : GL_FALSE,
						layout.GetStride(),
						(const void*)(element.Offset + sizeof(float) * count * i));
					glVertexAttribDivisor(mVertexBufferIndex, 1);
					mVertexBufferIndex++;
				}
				break;
			}
			default:
				HE_CORE_ASSERT(false, "Unknown ShaderDataType!");
			}
        }

        mVertexBuffers.push_back(vertexBuffer);
    }

    void OpenGLVertexArray::SetIndexBuffer(const Ref<IndexBuffer>& indexBuffer)
    {
        glBindVertexArray(mRendererID);
        indexBuffer->Bind();

        mIndexBuffer = indexBuffer;
    }
}
