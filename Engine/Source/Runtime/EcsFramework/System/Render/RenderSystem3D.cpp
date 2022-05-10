#include "hepch.h"

#include "Runtime/EcsFramework/System/Render/RenderSystem3D.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Renderer/Renderer3D.h"
#include "Runtime/Library/ShaderLibrary.h"

namespace HEngine
{
    void RenderSystem3D::OnUpdateRuntime(Timestep ts)
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

		if (mainCamera)
		{
			Renderer3D::BeginScene(*mainCamera, cameraTransform);

			// Get the Light 
			{

			}

			// Draw model
			{
				auto view = mLevel->mRegistry.view<TransformComponent, StaticMeshComponent>();
				for (auto entity : view)
				{
					auto [transform, mesh] = view.get<TransformComponent, StaticMeshComponent>(entity);

					Renderer3D::DrawModel(transform.GetTransform(), cameraPos, mesh, (int)entity);
				}
			}

			Renderer3D::EndScene();
		}
    }

	void RenderSystem3D::OnUpdateEditor(Timestep ts, EditorCamera& camera)
	{
		Renderer3D::BeginScene(camera);

		// Get the Light 
		{
			auto view = mLevel->mRegistry.view<TransformComponent, LightComponent>();
			int i = 0;
			for (auto entity : view)
			{
				auto [transform, light] = view.get<TransformComponent, LightComponent>(entity);

				glm::vec3 lightPos = transform.GetTranslation();
				glm::vec3 lightColor = light.LightColor;

				Ref<Shader> iblPbr = Library<Shader>::GetInstance().Get("IBL_pbr");
				Ref<Shader> defaultPbr = Library<Shader>::GetInstance().GetDefaultShader();

				iblPbr->Bind();
				iblPbr->SetFloat3("lightPositions[" + std::to_string(i) + "]", lightPos);
				iblPbr->SetFloat3("lightColors[" + std::to_string(i) + "]", lightColor);				
				
				defaultPbr->Bind();
				defaultPbr->SetFloat3("lightPositions[" + std::to_string(i) + "]", lightPos);
				defaultPbr->SetFloat3("lightColors[" + std::to_string(i) + "]", lightColor);

				i++;
			}
		}

		auto view = mLevel->mRegistry.view<TransformComponent, StaticMeshComponent>();
		for (auto e : view)
		{
			Entity entity = { e, mLevel };
			auto& transform = entity.GetComponent<TransformComponent>();
			auto& mesh = entity.GetComponent<StaticMeshComponent>();

			Renderer3D::DrawModel(transform.GetTransform(), camera.GetPosition(), mesh, (int)e);
		}

		Renderer3D::EndScene();
	}
}