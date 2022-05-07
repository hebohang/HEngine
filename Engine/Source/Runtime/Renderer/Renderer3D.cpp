#include "hepch.h"

#include "Runtime/Renderer/Renderer3D.h"
#include "Runtime/Renderer/Texture.h"
#include "Runtime/Renderer/VertexArray.h"
#include "Runtime/Renderer/Shader.h"
#include "Runtime/Renderer/RenderCommand.h"
#include "Runtime/Renderer/UniformBuffer.h"
#include "Runtime/Library/ShaderLibrary.h"
#include "Runtime/Library/UniformBufferLibrary.h"

#include "Runtime/Resource/AssetManager/AssetManager.h"

#include <glm/glm.hpp>

namespace HEngine
{
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
		sSkyBoxShader = Shader::Create(AssetManager::GetFullPath("Shaders/SkyBox.glsl"));
		sSkyBox = CubeMapTexture::Create(sPaths);

		sBox = Model(AssetManager::GetFullPath("Assets/Models/Box.obj").string());
	}

	void Renderer3D::Shutdown()
	{
	}

	void Renderer3D::DrawModel(const glm::mat4& transform, const glm::vec3& cameraPos, StaticMeshComponent& MeshComponent, int EntityID)
	{
		MeshComponent.Mesh.Draw(transform, cameraPos, EntityID);
	}

	void Renderer3D::BeginScene(const Camera& camera, const glm::mat4& transform)
	{
		Ref<UniformBuffer> cameraUniform = UniformBufferLibrary::GetInstance().GetCameraUniformBuffer();
		glm::mat4 ViewProjection = camera.GetProjection() * glm::inverse(transform);
		cameraUniform->SetData(&ViewProjection, sizeof(ViewProjection));
	}

	void Renderer3D::BeginScene(const EditorCamera& camera)
	{
		Ref<UniformBuffer> cameraUniform = UniformBufferLibrary::GetInstance().GetCameraUniformBuffer();
		glm::mat4 ViewProjection = camera.GetViewProjection();
		cameraUniform->SetData(&ViewProjection, sizeof(ViewProjection));
	}

	void Renderer3D::EndScene()
	{
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
		Ref<UniformBuffer> cameraUniform = UniformBufferLibrary::GetInstance().GetCameraUniformBuffer();
		glm::mat4 ViewProjection = camera.GetProjection() * glm::mat4(glm::mat3(camera.GetViewMatrix()));
		cameraUniform->SetData(&ViewProjection, sizeof(ViewProjection));

		RenderCommand::Cull(0);

		RenderCommand::DepthFunc(DepthComp::LEQUAL);
		sSkyBoxShader->Bind();

		sSkyBox->Bind(0);
		sSkyBoxShader->SetInt("SkyBox", 0);
		sBox.Draw();

		RenderCommand::DepthFunc(DepthComp::LESS);
	}
}