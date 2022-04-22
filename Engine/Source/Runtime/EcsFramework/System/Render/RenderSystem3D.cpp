#include "hepch.h"

#include "Runtime/EcsFramework/System/Render/RenderSystem3D.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/Entity/Entity.h"

#include "Runtime/Renderer/Renderer3D.h"

namespace HEngine
{
    void RenderSystem3D::OnUpdateRuntime(Timestep ts)
    {
    }

	void RenderSystem3D::OnUpdateEditor(Timestep ts, EditorCamera& camera)
	{
		Renderer3D::BeginScene(camera);

		auto group = mLevel->mRegistry.group<TransformComponent>(entt::get<StaticMeshComponent>);

		for (auto entity : group)
		{
			auto [transform, mesh] = group.get<TransformComponent, StaticMeshComponent>(entity);

			Renderer3D::DrawModel(transform.GetTransform(), mesh, (int)entity);
		}

		Renderer3D::EndScene();
	}
}