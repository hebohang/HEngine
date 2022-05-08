#include "hepch.h"

#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/EcsFramework/System/Render/EnvironmentSystem.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Renderer/RenderCommand.h"
#include "Runtime/Library/UniformBufferLibrary.h"
#include "Runtime/Library/ShaderLibrary.h"
#include "Runtime/Renderer/Renderer3D.h"

#include <glm/glm.hpp>

namespace HEngine
{
	void EnvironmentSystem::OnUpdateRuntime(Timestep ts)
	{
		Camera* mainCamera = nullptr;
		glm::mat4 cameraTransform;
		glm::vec3 cameraPos;
		{
			auto view = mLevel->mRegistry.view<TransformComponent, CameraComponent>();
			for (auto entity : view)
			{
				auto [transform, camera] = view.get<TransformComponent, CameraComponent>(entity);

				if (camera.Primary)
				{
					mainCamera = &camera.Camera;
					cameraTransform = transform.GetTransform();
					cameraPos = transform.GetTranslation();
					break;
				}
			}
		}

		switch (ModeManager::mSceneMode)
		{
		case SceneMode::None:
			break;
		case SceneMode::EnvironmentHdr:
			break;
		case SceneMode::SkyBox:
			if (mainCamera)
				DrawSkyBox(glm::inverse(cameraTransform), mainCamera->GetProjection());
			break;
		default:
			break;
		}
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
			DrawSkyBox(camera.GetViewMatrix(), camera.GetProjection());
			break;
		default:
			break;
		}
	}

	void EnvironmentSystem::DrawSkyBox(const glm::mat4& ViewMatrix, const glm::mat4& ProjectMatrix)
	{
		Ref<UniformBuffer> cameraUniform = Library<UniformBuffer>::GetInstance().GetCameraUniformBuffer();
		glm::mat4 ViewProjection = ProjectMatrix * glm::mat4(glm::mat3(ViewMatrix));
		cameraUniform->SetData(&ViewProjection, sizeof(ViewProjection));

		RenderCommand::Cull(0);

		RenderCommand::DepthFunc(DepthComp::LEQUAL);
		Library<Shader>::GetInstance().GetSkyBoxShader()->Bind();

		Library<CubeMapTexture>::GetInstance().Get("SkyBoxTexture")->Bind(0);
		Library<Shader>::GetInstance().GetSkyBoxShader()->SetInt("SkyBox", 0);
		Library<Model>::GetInstance().Get("Box")->Draw();

		RenderCommand::DepthFunc(DepthComp::LESS);
	}
}