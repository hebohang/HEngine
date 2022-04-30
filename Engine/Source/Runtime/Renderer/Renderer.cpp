#include "hepch.h"

#include "Runtime/Renderer/Renderer.h"
#include "Runtime/Renderer/Renderer2D.h"
#include "Runtime/Renderer/Renderer3D.h"
#include "Runtime/Platform/OpenGL/OpenGLShader.h"

#include "Runtime/Core/AppFramework/Application.h"

namespace HEngine
{
    Renderer::SceneData* Renderer::mSceneData = new Renderer::SceneData;

    void Renderer::Init()
    {
        //RenderCommand::Init();
        //Renderer3D::Init();
        //Renderer2D::Init();
    }

	void Renderer::Shutdown()
	{
		//Renderer2D::Shutdown();
		//Renderer3D::Shutdown();
	}

    void Renderer::OnWindowResize(uint32_t width, uint32_t height)
    {
        RenderCommand::SetViewport(0, 0, width, height);
    }

    void Renderer::EndScene()
    {
    }

    void Renderer::Submit(const Ref<Shader>& shader, const Ref<VertexArray>& vertexArray, const glm::mat4& transform)
    {
        shader->Bind();
        std::dynamic_pointer_cast<OpenGLShader>(shader)->UploadUniformMat4("u_ViewProjection", mSceneData->ViewProjectionMatrix);
        std::dynamic_pointer_cast<OpenGLShader>(shader)->UploadUniformMat4("u_Transform", transform);

        vertexArray->Bind();
        RenderCommand::DrawIndexed(vertexArray);
    }

    Ref<GraphicsContext> Renderer::GetContext()
    {
        return Application::GetInstance().GetWindow().GetGraphicsContext();
    }
}
