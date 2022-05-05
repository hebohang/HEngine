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
	static Ref<Shader> sShader;

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

	static Ref<CubeMapTexture> sSkyBox;
	static Ref<Shader> sSkyBoxShader;
	static Model sBox;

	std::vector<std::string> sPaths{ 
		"Assets/Textures/Skybox/right.jpg",
		"Assets/Textures/Skybox/left.jpg",
		"Assets/Textures/Skybox/top.jpg",
		"Assets/Textures/Skybox/bottom.jpg",
		"Assets/Textures/Skybox/front.jpg",
		"Assets/Textures/Skybox/back.jpg",
	};

	void Renderer3D::Init()
	{
		sShader = Shader::Create(AssetManager::GetInstance().GetFullPath("Shaders/Common.glsl"));
		sData.CameraUniformBuffer = UniformBuffer::Create(sizeof(Renderer3DData::CameraData), 1);

		sSkyBoxShader = Shader::Create(AssetManager::GetInstance().GetFullPath("Shaders/SkyBox.glsl"));
		sSkyBox = CubeMapTexture::Create(sPaths);

		sBox = Model(AssetManager::GetInstance().GetFullPath("Assets/Models/Box.obj").string());
	}

	void Renderer3D::Shutdown()
	{
	}

	void Renderer3D::DrawModel(const glm::mat4& transform, StaticMeshComponent& MeshComponent, int EntityID)
	{
		MeshComponent.Mesh.Draw(transform, sShader, EntityID);
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
		sShader->Unbind();
	}

	Ref<CubeMapTexture> Renderer3D::GetSkyBox()
	{
		return sSkyBox;
	}

	Ref<CubeMapTexture> Renderer3D::GetDefaultSkyBox()
	{
		sSkyBox = CubeMapTexture::Create(sPaths);
		return sSkyBox;
	}

	void Renderer3D::DrawSkyBox(const EditorCamera& camera)
	{
		sData.CameraBuffer.ViewProjection = camera.GetProjection() * glm::mat4(glm::mat3(camera.GetViewMatrix()));
		sData.CameraUniformBuffer->SetData(&sData.CameraBuffer, sizeof(Renderer3DData::CameraData), 0);

		RenderCommand::Cull(0);

		RenderCommand::DepthFunc(DepthComp::LEQUAL);
		sSkyBoxShader->Bind();

		sSkyBox->Bind(0);
		sSkyBoxShader->SetInt("SkyBox", 0);
		sBox.Draw();

		RenderCommand::DepthFunc(DepthComp::LESS);
	}
}