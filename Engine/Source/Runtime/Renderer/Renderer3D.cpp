#include "hepch.h"

#include "Runtime/Renderer/Renderer3D.h"
#include "Runtime/Renderer/Texture.h"
#include "Runtime/Renderer/VertexArray.h"
#include "Runtime/Renderer/Shader.h"
#include "Runtime/Renderer/RenderCommand.h"

#include "Runtime/Resource/AssetManager/AssetManager.h"

#include <glm/glm.hpp>

namespace HEngine
{
	static Ref<Shader> mShader;

	void Renderer3D::Init()
	{
		mShader = Shader::Create(AssetManager::GetInstance().GetFullPath("Shaders/Common.glsl"));
	}

	void Renderer3D::Shutdown()
	{
	}

	void Renderer3D::DrawModel(const glm::mat4& transform, StaticMeshComponent& MeshComponent, int EntityID)
	{
		MeshComponent.mesh.Draw(transform, mShader, EntityID);
	}

	void Renderer3D::BeginScene(const Camera& camera, const glm::mat4& transform)
	{
		glm::mat4 viewProj = camera.GetProjection() * glm::inverse(transform);

		mShader->Bind();
		mShader->SetMat4("u_ViewProjection", viewProj);
	}

	void Renderer3D::BeginScene(const EditorCamera& camera)
	{
		glm::mat4 viewProj = camera.GetViewProjection();

		mShader->Bind();
		mShader->SetMat4("u_ViewProjection", viewProj);
	}

	void Renderer3D::EndScene()
	{
		mShader->Unbind();
	}
}