#include "hepch.h"
#include "Framebuffer.h"

#include "Runtime/Renderer/Renderer.h"

#include "Platform/OpenGL/OpenGLFramebuffer.h"

namespace HEngine
{
    Ref<Framebuffer> Framebuffer::Create(const FramebufferSpecification& spec)
    {
        switch (RendererAPI::Current())
        {
        case RendererAPI::RendererAPIType::None:    HE_CORE_ASSERT(false, "RendererAPI::None is currently not supported!"); return nullptr;
        case RendererAPI::RendererAPIType::OpenGL:  return CreateRef<OpenGLFramebuffer>(spec);
        }
        HE_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
}