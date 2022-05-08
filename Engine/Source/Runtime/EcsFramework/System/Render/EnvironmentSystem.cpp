#include "hepch.h"

#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/EcsFramework/System/Render/EnvironmentSystem.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Renderer/RenderCommand.h"
#include "Runtime/Library/UniformBufferLibrary.h"

#include "Runtime/Renderer/Renderer3D.h"

namespace HEngine
{
	void EnvironmentSystem::OnUpdateRuntime(Timestep ts)
	{

	}

	void EnvironmentSystem::OnUpdateEditor(Timestep ts, EditorCamera& camera)
	{
		switch (ModeManager::mSceneMode)
		{
		case SceneMode::None:
			break;
		case SceneMode::EnvironmentHdr:
			break;
		case SceneMode::SkyBox:
			Renderer3D::DrawSkyBox(camera);
			break;
		default:
			break;
		}
	}

	void EnvironmentSystem::DrawSkyBox(const glm::mat4& ViewMatrix, const glm::mat4& ProjectMatrix, Ref<UniformBuffer> CameraUniformBuffer)
	{
		//Ref<UniformBuffer> cameraUniform = UniformBufferLibrary::GetInstance().GetCameraUniformBuffer();
		//glm::mat4 ViewProjection = camera.GetProjection() * glm::mat4(glm::mat3(camera.GetViewMatrix()));
		//cameraUniform->SetData(&ViewProjection, sizeof(ViewProjection));

		RenderCommand::Cull(0);

		RenderCommand::DepthFunc(DepthComp::LEQUAL);
		//sSkyBoxShader->Bind();

		//sSkyBox->Bind(0);
		//sSkyBoxShader->SetInt("SkyBox", 0);
		//sBox.Draw();

		RenderCommand::DepthFunc(DepthComp::LESS);
	}
}