#include "Hepch.h"

#include "Runtime/EcsFramework/System/Render/RenderSystem3D.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Graphics/Renderer2D.h"
#include "Runtime/Graphics/Settings/RenderSettingsService.h"
#include "Runtime/Graphics/VFX/EffekseerRuntime.h"

#if WITH_EDITOR
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"
#endif

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

namespace HEngine
{
	namespace
	{
		[[nodiscard]] uint64_t AllocateRenderSceneGeneration() noexcept
		{
			static std::atomic<uint64_t> nextGeneration{ 1 };
			uint64_t generation = nextGeneration.fetch_add(1, std::memory_order_relaxed);
			if (generation == 0 || generation == std::numeric_limits<uint64_t>::max())
			{
				nextGeneration.store(2, std::memory_order_relaxed);
				generation = 1;
			}
			return generation;
		}

		void AdvanceGeneration(uint64_t& generation) noexcept
		{
			++generation;
			if (generation == 0)
				generation = 1;
		}
	}

	RenderSystem3D::RenderSystem3D(Level* level)
		: System(level), mSceneGeneration(AllocateRenderSceneGeneration())
	{
	}

	void RenderSystem3D::NotifyJitterSequenceReset() noexcept
	{
		AdvanceGeneration(mJitterSequenceGeneration);
	}

	void RenderSystem3D::ExtractAndSubmit(RenderViewDescription view)
	{
		HE_PROFILE_SECTION("Extract Render World");

		view.FrameNumber = ++mRenderFrameNumber;
		if (!bHasPreviousProjection)
		{
			mPreviousProjection = view.Projection;
			bHasPreviousProjection = true;
		}
		else if (std::memcmp(
			&mPreviousProjection,
			&view.Projection,
			sizeof(glm::mat4)) != 0)
		{
			mPreviousProjection = view.Projection;
			AdvanceGeneration(mProjectionGeneration);
		}
		view.SceneGeneration = mSceneGeneration;
		view.JitterSequenceGeneration = mJitterSequenceGeneration;
		view.ProjectionGeneration = mProjectionGeneration;
		view.bCameraCut = view.bCameraCut || bPendingCameraCut;

		RenderSettingsService& renderSettings =
			RenderSettingsService::GetInstance();
		if (renderSettings.IsInitialized())
		{
			const ResolvedRenderSettings settings =
				renderSettings.GetSnapshot();
			view.ProjectPostProcess.Exposure =
				settings.Effective.PostProcess.Exposure;
			view.bShadowsEnabled = settings.Effective.Shadows.bEnabled;
		}

		// This is the only ECS-facing stage of the modern renderer. The submitted
		// RenderWorld owns an immutable snapshot and is consumed after ECS update.
		RenderWorld world = mRenderWorldExtractor.Extract(*mLevel, view);
		RenderPrimitive2DFrame primitiveFrame = Renderer2D::TakeFrameSnapshot();
		world.Primitives2D = std::move(primitiveFrame.Primitives);
		mRenderWorldSubmission.Submit(std::move(world));
		bPendingCameraCut = false;
	}

	void RenderSystem3D::OnRuntiemStart()
	{
		mRenderWorldExtractor.Reset();
		mRenderWorldSubmission.Reset();
		mRenderFrameNumber = 0;
		mSceneGeneration = AllocateRenderSceneGeneration();
		bHasPreviousProjection = false;
		bPendingCameraCut = true;
	}

	void RenderSystem3D::OnRuntimeStop()
	{
		mRenderWorldExtractor.Reset();
		mRenderWorldSubmission.Reset();
		mRenderFrameNumber = 0;
		mSceneGeneration = AllocateRenderSceneGeneration();
		bHasPreviousProjection = false;
		bPendingCameraCut = true;
	}

	void RenderSystem3D::OnUpdateRuntime(Timestep ts)
	{
		HE_PROFILE;

		Camera* mainCamera = nullptr;
		SceneCamera* mainSceneCamera = nullptr;
		glm::mat4 cameraTransform{ 1.0f };
		glm::vec3 cameraPosition{ 0.0f };
		std::optional<float> cameraExposureOverride;
		{
			HE_PROFILE_SECTION("Find Main Camera");
			auto cameras = mLevel->mRegistry.view<TransformComponent, CameraComponent>();
			for (const auto entity : cameras)
			{
				Entity cameraEntity{ entity, mLevel };
				auto& cameraComponent = cameras.get<CameraComponent>(entity);
				if (!cameraComponent.Primary)
					continue;

				mainCamera = &cameraComponent.Camera;
				mainSceneCamera = &cameraComponent.Camera;
				cameraTransform = mLevel->GetWorldTransform(cameraEntity);
				cameraPosition = mLevel->GetWorldTranslation(cameraEntity);
				if (cameraComponent.bOverrideExposure)
					cameraExposureOverride = cameraComponent.Exposure;
				break;
			}
		}

		if (!mainCamera)
		{
			// Do not let a consumer render a stale snapshot after the active camera
			// has been removed from the level.
			mRenderWorldSubmission.Reset();
			(void)Renderer2D::TakeFrameSnapshot();
			return;
		}

		const glm::mat4 view = glm::inverse(cameraTransform);
		const glm::mat4 projection = mainCamera->GetProjection();
		RenderViewDescription renderView;
		renderView.CameraPosition = cameraPosition;
		renderView.View = view;
		renderView.Projection = projection;
		renderView.CameraExposureOverride = cameraExposureOverride;
		if (mainSceneCamera)
		{
			renderView.NearPlane = mainSceneCamera->GetProjectionType() == SceneCamera::ProjectionType::Perspective
				? mainSceneCamera->GetPerspectiveNearClip()
				: std::max(0.001f, mainSceneCamera->GetOrthographicNearClip());
			renderView.FarPlane = mainSceneCamera->GetProjectionType() == SceneCamera::ProjectionType::Perspective
				? mainSceneCamera->GetPerspectiveFarClip()
				: mainSceneCamera->GetOrthographicFarClip();
		}

		{
			HE_PROFILE_SECTION("Effekseer Update");
			if (!mLevel->IsRuntimePaused())
				EffekseerRuntime::Update(ts);
			EffekseerRuntime::SetCamera(view, projection);
		}
		ExtractAndSubmit(renderView);
	}

#if WITH_EDITOR
	bool RenderSystem3D::RequiresContinuousEditorUpdate() const
	{
		if (ModeManager::bRealtimePreview && EffekseerRuntime::HasActiveEffects())
			return true;

		const auto meshView = mLevel->mRegistry.view<MeshComponent>();
		for (const auto entity : meshView)
		{
			const MeshComponent& mesh = meshView.get<MeshComponent>(entity);
			if (!mesh.mMesh)
				continue;
			for (const Ref<Material>& material : mesh.mMesh->mMaterial)
			{
				if (material && material->IsCustomSurfaceShader())
					return true;
			}
		}
		return false;
	}

	void RenderSystem3D::OnUpdateEditor(Timestep ts, EditorCamera& camera)
	{
		SubmitEditorView(ts, camera, std::nullopt);
	}

	void RenderSystem3D::OnUpdateEditorAtMaterialTime(
		Timestep ts,
		EditorCamera& camera,
		double materialTimeSeconds)
	{
		SubmitEditorView(ts, camera, materialTimeSeconds);
	}

	void RenderSystem3D::SubmitEditorView(
		Timestep ts,
		EditorCamera& camera,
		std::optional<double> materialTimeSeconds)
	{
		RenderViewDescription view;
		view.View = camera.GetViewMatrix();
		view.Projection = camera.GetProjection();
		view.CameraPosition = camera.GetPosition();
		view.NearPlane = camera.GetNearPlane();
		view.FarPlane = camera.GetFarPlane();
		view.SelectedEntity = ConfigManager::selectedEntity;
		view.bEditorView = true;
		view.MaterialTimeSeconds = materialTimeSeconds;

		EffekseerRuntime::Update(ts);
		EffekseerRuntime::SetCamera(view.View, view.Projection);
		ExtractAndSubmit(view);
	}
#endif
}
