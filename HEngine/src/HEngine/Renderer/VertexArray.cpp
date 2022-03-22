#include "hepch.h"
#include "VertexArray.h"

#include "Renderer.h"
#include "Platform/OpenGL/OpenGLVertexArray.h"

namespace HEngine
{
    VertexArray* VertexArray::Create()
    {
        switch (Renderer::GetAPI())
        {
        case RendererAPI::None:    HE_CORE_ASSERT(false, "RendererAPI::None is currently not supported!"); return nullptr;
        case RendererAPI::OpenGL:  return new OpenGLVertexArray();
        }
        HE_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
}