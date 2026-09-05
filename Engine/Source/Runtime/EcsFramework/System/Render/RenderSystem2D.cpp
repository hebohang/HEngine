#include "Hepch.h"

#include "Runtime/EcsFramework/System/Render/RenderSystem2D.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Component/Shape/CircleRendererComponent.h"
#include "Runtime/EcsFramework/Component/Shape/SpriteRendererComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"

#include "Runtime/Graphics/Renderer2D.h"

#include <utility>

namespace HEngine
{
	void RenderSystem2D::SubmitPrimitiveFrame(RenderPrimitive2DFrame frame, bool bEditorView)
	{
		if (!frame.bHasCamera)
		{
			mRenderWorldSubmission.Reset();
			return;
		}

		RenderWorld world;
		world.Camera = frame.Camera;
		world.Primitives2D = std::move(frame.Primitives);
		world.FrameNumber = ++mRenderFrameNumber;
		world.bEditorView = bEditorView;
		mRenderWorldSubmission.Submit(std::move(world));
	}

	void RenderSystem2D::OnRuntiemStart()
	{
		mRenderWorldSubmission.Reset();
		mRenderFrameNumber = 0;
	}

	void RenderSystem2D::OnRuntimeStop()
	{
		mRenderWorldSubmission.Reset();
		mRenderFrameNumber = 0;
	}

    void RenderSystem2D::OnUpdateRuntime(Timestep ts)
    {
		HE_PROFILE;
		(void)ts;

		// Render 2D
		Camera* mainCamera = nullptr;
		glm::mat4 cameraTransform;
		{
			auto view = mLevel->mRegistry.view<TransformComponent, CameraComponent>();
			for (auto entity : view)
			{
				Entity cameraEntity = { entity, mLevel };
				auto& camera = view.get<CameraComponent>(entity);

				if (camera.Primary)
				{
					mainCamera = &camera.Camera;
					cameraTransform = mLevel->GetWorldTransform(cameraEntity);
					break;
				}
			}
		}

		if (!mainCamera)
		{
			(void)Renderer2D::TakeFrameSnapshot();
			mRenderWorldSubmission.Reset();
			return;
		}

		Renderer2D::BeginScene(
			*mainCamera,
			cameraTransform,
			RenderPrimitive2DCategory::Scene,
			RenderPrimitiveDepthMode::DepthTest);

		// Draw sprites
		{
			auto group = mLevel->mRegistry.group<TransformComponent>(entt::get<SpriteRendererComponent>);
			for (auto entity : group)
			{
				Entity spriteEntity = { entity, mLevel };
				auto& sprite = group.get<SpriteRendererComponent>(entity);
				const glm::mat4 transform = mLevel->GetWorldTransform(spriteEntity);
				const int entityId = static_cast<int>(static_cast<uint32_t>(entity));
				if (sprite.Texture.empty())
					Renderer2D::DrawQuad(transform, sprite.Color, entityId);
				else
					Renderer2D::DrawQuad(
						transform,
						sprite.Texture,
						sprite.TilingFactor,
						sprite.Color,
						entityId);
			}
		}

		// Draw circles
		{
			auto view = mLevel->mRegistry.view<TransformComponent, CircleRendererComponent>();
			for (auto entity : view)
			{
				Entity circleEntity = { entity, mLevel };
				auto& circle = view.get<CircleRendererComponent>(entity);
				Renderer2D::DrawCircle(
					mLevel->GetWorldTransform(circleEntity),
					circle.Color,
					circle.Thickness,
					circle.Fade,
					static_cast<int>(static_cast<uint32_t>(entity)));
			}
		}

		Renderer2D::EndScene();
		SubmitPrimitiveFrame(Renderer2D::TakeFrameSnapshot(), false);
	}

#if WITH_EDITOR
	void RenderSystem2D::OnUpdateEditor(Timestep ts, EditorCamera& camera)
	{
		(void)ts;
		Renderer2D::BeginScene(
			camera,
			RenderPrimitive2DCategory::Scene,
			RenderPrimitiveDepthMode::DepthTest);

		// Draw sprites
		{
			auto group = mLevel->mRegistry.view<TransformComponent, SpriteRendererComponent>();
			for (auto entity : group)
			{
				Entity spriteEntity = { entity, mLevel };
				if (!mLevel->IsEntityVisibleInEditor(spriteEntity))
					continue;
				auto& sprite = group.get<SpriteRendererComponent>(entity);
				const glm::mat4 transform = mLevel->GetWorldTransform(spriteEntity);
				const int entityId = static_cast<int>(static_cast<uint32_t>(entity));
				if (sprite.Texture.empty())
					Renderer2D::DrawQuad(transform, sprite.Color, entityId);
				else
					Renderer2D::DrawQuad(
						transform,
						sprite.Texture,
						sprite.TilingFactor,
						sprite.Color,
						entityId);
			}
		}

		// Draw circle
		{
			auto view = mLevel->mRegistry.view<TransformComponent, CircleRendererComponent>();
			for (auto entity : view)
			{
				Entity circleEntity = { entity, mLevel };
				if (!mLevel->IsEntityVisibleInEditor(circleEntity))
					continue;
				auto& circle = view.get<CircleRendererComponent>(entity);
				Renderer2D::DrawCircle(
					mLevel->GetWorldTransform(circleEntity),
					circle.Color,
					circle.Thickness,
					circle.Fade,
					static_cast<int>(static_cast<uint32_t>(entity)));
			}
		}

		Renderer2D::EndScene();
		SubmitPrimitiveFrame(Renderer2D::TakeFrameSnapshot(), true);
	}
#endif
}
