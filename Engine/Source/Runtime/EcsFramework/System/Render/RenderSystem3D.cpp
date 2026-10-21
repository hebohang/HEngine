#include "Hepch.h"
#include "Runtime/Graphics/Lighting/PhotometricUnits.h"
#include "Runtime/Graphics/Scene/CameraView.h"

#include "Runtime/EcsFramework/System/Render/RenderSystem3D.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Graphics/Renderer2D.h"
#include "Runtime/Graphics/Settings/RenderSettingsService.h"
#include "Runtime/Graphics/VFX/EffekseerRuntime.h"
#include "Runtime/Core/SimulationClock.h"

#if WITH_EDITOR
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_set>
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

    Ref<RenderResourceWarmupRequest> RenderSystem3D::PrepareMeshResources(const Ref<Mesh>& mesh)
    {
        if (!mesh) throw std::invalid_argument("Render resource warmup requires a mesh.");
        auto request = CreateRef<RenderResourceWarmupRequest>();
        request->MeshResource = mesh;
        mResourceWarmups.push_back({ request });
        return request;
    }

    std::vector<Ref<RenderResourceWarmupRequest>> RenderSystem3D::PrepareLevelMeshResources()
    {
        std::vector<Ref<RenderResourceWarmupRequest>> requests;
        if (!mLevel) return requests;
        const auto meshes = mLevel->mRegistry.view<MeshComponent>();
        std::unordered_set<const Mesh*> uniqueMeshes;
        uniqueMeshes.reserve(meshes.size());
        requests.reserve(meshes.size());
        for (auto entity : meshes)
        {
            const auto& mesh = meshes.get<MeshComponent>(entity).mMesh;
            if (mesh && !mesh->mSubMeshes.empty() && uniqueMeshes.insert(mesh.get()).second)
                requests.push_back(PrepareMeshResources(mesh));
        }
        return requests;
    }

	void RenderSystem3D::NotifyJitterSequenceReset() noexcept
	{
		AdvanceGeneration(mJitterSequenceGeneration);
	}

	void RenderSystem3D::ExtractAndSubmit(RenderViewDescription view)
	{
		HE_PROFILE_SECTION("Extract Render World");

		view.FrameNumber = mRenderFrameNumber + 1;
		uint64_t projectionGeneration = mProjectionGeneration;
		if (bHasPreviousProjection && std::memcmp(
			&mPreviousProjection, &view.Projection, sizeof(glm::mat4)) != 0)
			AdvanceGeneration(projectionGeneration);
		view.SceneGeneration = mSceneGeneration;
		view.JitterSequenceGeneration = mJitterSequenceGeneration;
		view.ProjectionGeneration = projectionGeneration;
		view.bCameraCut = view.bCameraCut || bPendingCameraCut;

		RenderSettingsService& renderSettings =
			RenderSettingsService::GetInstance();
		if (renderSettings.IsInitialized())
		{
			const ResolvedRenderSettings settings =
				renderSettings.GetSnapshot();
			view.ProjectPostProcess.Exposure =
				settings.Effective.PostProcess.Exposure;
            view.ProjectPostProcess.BloomIntensity = settings.Effective.PostProcess.BloomIntensity;
            view.ProjectPostProcess.Contrast = settings.Effective.PostProcess.Contrast;
            view.ProjectPostProcess.Saturation = settings.Effective.PostProcess.Saturation;
		}

		// This is the only ECS-facing stage of the modern renderer. The submitted
		// RenderWorld owns an immutable snapshot and is consumed after ECS update.
		RenderWorld world;
		try
		{
			world = mRenderWorldExtractor.Extract(*mLevel, view);
            std::erase_if(mResourceWarmups, [](const WarmupSnapshot& item) { return item.Request.expired(); });
            for (auto& item : mResourceWarmups)
            {
                const auto request = item.Request.lock();
                if (!request || !request->MeshResource) continue;
                const auto& mesh = request->MeshResource;
                // An already-visible instance is prepared by the normal path.
                if (std::ranges::any_of(world.Objects, [&](const RenderObject& object) {
                    return object.MeshResource == mesh && object.bVisible;
                })) continue;
                bool bCurrent = item.Object && item.SceneGeneration == world.SceneGeneration &&
                    item.Object->Sections.size() == mesh->mSubMeshes.size();
                for (size_t index = 0; bCurrent && index < mesh->mSubMeshes.size(); ++index)
                {
                    const auto& subMesh = mesh->mSubMeshes[index];
                    const auto material = subMesh && subMesh->mMaterialIndex < mesh->mMaterial.size()
                        ? mesh->mMaterial[subMesh->mMaterialIndex] : Ref<Material>{};
                    const auto& section = item.Object->Sections[index];
                    bCurrent = section.SubMeshResource == subMesh && section.MaterialResource == material &&
                        (!material || section.GetMaterialInstance().Revision == material->GetRuntimeRevision());
                }
                if (!bCurrent)
                {
                    item.Object = mRenderWorldExtractor.BuildResourceWarmupObject(mesh);
                    item.SceneGeneration = world.SceneGeneration;
                }
                world.ResourceWarmupObjects.push_back(item.Object);
            }
		}
		catch (const std::invalid_argument& error)
		{
			if (mLastExtractionError != error.what())
			{
				mLastExtractionError = error.what();
				HE_CORE_ERROR("RenderSystem3D: render world extraction rejected; retaining the previous submitted world: {0}", mLastExtractionError);
			}
			// Level must close its collector each update. Preserve its pending data
			// here until a valid world can own it, instead of discarding the frame.
			auto pending = Renderer2D::TakeFrameSnapshot();
			// Primitives describe one frame, not accumulated commands. The latest
			// rejected frame replaces the previous one, bounding retained storage.
			mPendingPrimitives2D = std::move(pending.Primitives);
			return;
		}
		RenderPrimitive2DFrame primitiveFrame = Renderer2D::TakeFrameSnapshot();
		// Fresh draw data supersedes the rejected snapshot; otherwise replay the
		// latest pending snapshot once, without duplicating per-frame draws.
		world.Primitives2D = primitiveFrame.Primitives.empty()
			? mPendingPrimitives2D : std::move(primitiveFrame.Primitives);
		mRenderWorldSubmission.Submit(std::move(world));
		mRenderFrameNumber = view.FrameNumber;
		mProjectionGeneration = projectionGeneration;
		mPreviousProjection = view.Projection;
		bHasPreviousProjection = true;
		mLastExtractionError.clear();
		mPendingPrimitives2D.clear();
		bPendingCameraCut = false;
	}

	void RenderSystem3D::OnRuntiemStart()
	{
		mLastExtractionError.clear();
		mPendingPrimitives2D.clear();
		mRenderWorldExtractor.Reset();
		mRenderWorldSubmission.Reset();
		mRenderFrameNumber = 0;
		mSceneGeneration = AllocateRenderSceneGeneration();
		bHasPreviousProjection = false;
		bPendingCameraCut = true;
	}

	void RenderSystem3D::OnRuntimeStop()
	{
        mResourceWarmups.clear();
		mLastExtractionError.clear();
		mPendingPrimitives2D.clear();
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
		static const bool bTimeSystems = [] {
			const char* setting = std::getenv("HE_RUNTIME_SYSTEM_TIMINGS");
			return setting && (setting[0] == '1' || setting[0] == '2') &&
				setting[1] == '\0';
		}();
		const auto timingStart = bTimeSystems ? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};

		// Simulation belongs to the level update, even when no view can be extracted.
		if (!mLevel->IsRuntimePaused())
			EffekseerRuntime::Update(ts);
		const auto afterEffectUpdate = bTimeSystems ? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};

		Camera* mainCamera = nullptr;
		SceneCamera* mainSceneCamera = nullptr;
		glm::mat4 cameraTransform{ 1.0f };
		glm::vec3 cameraPosition{ 0.0f };
		std::optional<float> cameraExposureOverride;
        int activeCameraEntityId = -1;
		{
			HE_PROFILE_SECTION("Find Main Camera");
			auto cameras = mLevel->mRegistry.view<TransformComponent, CameraComponent>();
			for (const auto entity : cameras)
			{
				Entity cameraEntity{ entity, mLevel };
				auto& cameraComponent = cameras.get<CameraComponent>(entity);
				if (!cameraComponent.Primary)
					continue;

                activeCameraEntityId = static_cast<int>(static_cast<uint32_t>(cameraEntity));
				mainCamera = &cameraComponent.Camera;
				mainSceneCamera = &cameraComponent.Camera;
				cameraTransform = mLevel->GetWorldTransform(cameraEntity);
				cameraPosition = mLevel->GetWorldTranslation(cameraEntity);
				if (cameraComponent.bOverrideExposure)
					cameraExposureOverride = Photometry::ExposureMultiplier(cameraComponent.ExposureEV100, cameraComponent.ExposureCompensation);
				break;
			}
		}

		if (!mainCamera)
		{
			// Do not let a consumer render a stale snapshot after the active camera
			// has been removed from the level.
			mRenderWorldSubmission.Reset();
			mPendingPrimitives2D.clear();
			mLastExtractionError.clear();
			(void)Renderer2D::TakeFrameSnapshot();
			return;
		}

		const glm::mat4 view = BuildRigidCameraView(cameraTransform);
		const glm::mat4 projection = mainCamera->GetProjection();
		RenderViewDescription renderView;
        renderView.CameraEntityId = activeCameraEntityId;
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
			EffekseerRuntime::SetCamera(view, projection);
		}
		const auto beforeExtract = bTimeSystems ? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};
		ExtractAndSubmit(renderView);
		if (bTimeSystems)
		{
			const auto end = std::chrono::steady_clock::now();
			const double extractMilliseconds = std::chrono::duration<double, std::milli>(
				end - beforeExtract).count();
			if (extractMilliseconds > 14.0)
			{
				HE_CORE_WARN("[RuntimeRenderTiming] frame={} totalMs={} effectUpdateMs={} cameraMs={} extractSubmitMs={}",
					SimulationClock::GetInstance().GetFrameNumber(),
					std::chrono::duration<double, std::milli>(end - timingStart).count(),
					std::chrono::duration<double, std::milli>(afterEffectUpdate - timingStart).count(),
					std::chrono::duration<double, std::milli>(beforeExtract - afterEffectUpdate).count(),
					extractMilliseconds);
			}
		}
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

		EffekseerRuntime::SetCamera(view.View, view.Projection);
		ExtractAndSubmit(view);
	}
#endif
}
