#include "hepch.h"
#include "Runtime/Renderer/GraphicsContext.h"

#include "Runtime/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLContext.h"

namespace HEngine 
{
	Scope<GraphicsContext> GraphicsContext::Create(void* window)
	{
		switch (RendererAPI::Current())
		{
		case RendererAPI::RendererAPIType::None:    HE_CORE_ASSERT(false, "RendererAPI::None is currently not supported!"); return nullptr;
		case RendererAPI::RendererAPIType::OpenGL:  return CreateScope<OpenGLContext>(static_cast<GLFWwindow*>(window));
		}

		HE_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}
}