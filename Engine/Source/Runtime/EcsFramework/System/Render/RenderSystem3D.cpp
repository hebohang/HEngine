#include "hepch.h"

#include "Runtime/EcsFramework/System/Render/RenderSystem3D.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/Entity/Entity.h"

#include "Runtime/Renderer/Renderer3D.h"

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

		auto group = mLevel->mRegistry.group<TransformComponent>(entt::get<StaticMeshComponent>);

		for (auto entity : group)
		{
			auto [transform, mesh] = group.get<TransformComponent, StaticMeshComponent>(entity);

			Renderer3D::DrawModel(transform.GetTransform(), camera.GetPosition(), mesh, (int)entity);
		}

		Renderer3D::EndScene();

		Renderer3D::DrawSkyBox(camera);
	}
}