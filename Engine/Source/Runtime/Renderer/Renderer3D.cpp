#include "hepch.h"

#include "Runtime/Renderer/Renderer3D.h"
#include "Runtime/Renderer/Texture.h"
#include "Runtime/Renderer/VertexArray.h"
#include "Runtime/Renderer/Shader.h"
#include "Runtime/Renderer/RenderCommand.h"
#include "Runtime/Renderer/UniformBuffer.h"

#include "Runtime/Resource/AssetManager/AssetManager.h"

#include <glm/glm.hpp>

namespace HEngine
{
	static Ref<Shader> mShader;

	struct Renderer3DData
	{
		struct CameraData
		{
			glm::mat4 ViewProjection;
		};
		CameraData CameraBuffer;
		Ref<UniformBuffer> CameraUniformBuffer;
	};

	static Renderer3DData sData;

	void Renderer3D::Init()
	{
		mShader = Shader::Create(AssetManager::GetInstance().GetFullPath("Shaders/Common.glsl"));
		sData.CameraUniformBuffer = UniformBuffer::Create(sizeof(Renderer3DData::CameraData), 1);
	}

	void Renderer3D::Shutdown()
	{
	}

	void Renderer3D::DrawModel(const glm::mat4& transform, StaticMeshComponent& MeshComponent, int EntityID)
	{
		MeshComponent.Mesh.Draw(transform, mShader, EntityID);
	}

	void Renderer3D::BeginScene(const Camera& camera, const glm::mat4& transform)
	{
		sData.CameraBuffer.ViewProjection = camera.GetProjection() * glm::inverse(transform);
		sData.CameraUniformBuffer->SetData(&sData.CameraBuffer, sizeof(Renderer3DData::CameraData));
	}

	void Renderer3D::BeginScene(const EditorCamera& camera)
	{
		sData.CameraBuffer.ViewProjection = camera.GetViewProjection();
		sData.CameraUniformBuffer->SetData(&sData.CameraBuffer, sizeof(Renderer3DData::CameraData));
	}

	void Renderer3D::EndScene()
	{
		mShader->Unbind();
	}
}