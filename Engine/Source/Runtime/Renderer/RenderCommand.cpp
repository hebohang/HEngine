#include "hepch.h"

#include "Runtime/Renderer/RenderCommand.h"

#include "Platform/OpenGL/OpenGLRendererAPI.h"
#include "Platform/DirectX11/Dx11RendererAPI.h"

namespace HEngine
{
	Scope<RendererAPI> RenderCommand::sRendererAPI = CreateScope<Dx11RendererAPI>();
}
