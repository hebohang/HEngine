#include "hepch.h"
#include "Runtime/Renderer/RenderCommand.h"

#include "Platform/OpenGL/OpenGLRendererAPI.h"

namespace HEngine
{
	Scope<RendererAPI> RenderCommand::s_RendererAPI = CreateScope<OpenGLRendererAPI>();
}
