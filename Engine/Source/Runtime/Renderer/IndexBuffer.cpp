#include "hepch.h"

#include "Runtime/Renderer/IndexBuffer.h"
#include "Runtime/Renderer/Renderer.h"

#include "Platform/OpenGL/OpenGLIndexBuffer.h"

namespace HEngine
{
    Ref<IndexBuffer> IndexBuffer::Create(uint32_t count)
    {
        switch (Renderer::GetAPI())
        {
        case RendererAPI::API::None:    return nullptr;
        case RendererAPI::API::OpenGL:  return CreateRef<OpenGLIndexBuffer>(count);
        }
        HE_CORE_ASSERT(false, "Unknown RendererAPI");
        return nullptr;
    }

    Ref<IndexBuffer> IndexBuffer::Create(uint32_t* indices, uint32_t count)
    {
        switch (Renderer::GetAPI())
        {
        case RendererAPI::API::None:    return nullptr;
        case RendererAPI::API::OpenGL:  return CreateRef<OpenGLIndexBuffer>(indices, count);
        }
        HE_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
}