#include "hepch.h"
#include "Runtime/Renderer/VertexArray.h"

#include "Renderer.h"
#include "Platform/OpenGL/OpenGLVertexArray.h"

namespace HEngine
{
    Ref<VertexArray> VertexArray::Create()
    {
        switch (Renderer::GetAPI())
        {
        case RendererAPI::API::None:    HE_CORE_ASSERT(false, "RendererAPI::None is currently not supported!"); return nullptr;
        case RendererAPI::API::OpenGL:  return std::make_shared<OpenGLVertexArray>();
        }
        HE_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
}