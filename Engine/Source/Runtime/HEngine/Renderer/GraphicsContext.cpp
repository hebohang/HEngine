#include "hepch.h"
#include "HEngine/Renderer/GraphicsContext.h"

#include "HEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLContext.h"

namespace HEngine 
{
	Scope<GraphicsContext> GraphicsContext::Create(void* window)
	{
		switch (Renderer::GetAPI())
		{
		case RendererAPI::API::None:    HE_CORE_ASSERT(false, "RendererAPI::None is currently not supported!"); return nullptr;
		case RendererAPI::API::OpenGL:  return CreateScope<OpenGLContext>(static_cast<GLFWwindow*>(window));
		}

		HE_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}
}