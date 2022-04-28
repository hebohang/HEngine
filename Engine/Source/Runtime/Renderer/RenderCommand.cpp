#include "hepch.h"

#include "Runtime/Renderer/RenderCommand.h"

#include "Platform/OpenGL/OpenGLRendererAPI.h"

namespace HEngine
{
	Scope<RendererAPI> RenderCommand::sRendererAPI = CreateScope<OpenGLRendererAPI>();
}
