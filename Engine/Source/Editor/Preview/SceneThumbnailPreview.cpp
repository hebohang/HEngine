#include "Editor/Preview/SceneThumbnailPreview.h"

#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Component/Shape/CircleRendererComponent.h"
#include "Runtime/EcsFramework/Component/Shape/SpriteRendererComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/EcsFramework/System/Render/RenderSystem2D.h"
#include "Runtime/EcsFramework/System/Render/RenderSystem3D.h"
#include "Runtime/Graphics/Mesh/Mesh.h"
#include "Runtime/Graphics/Mesh/SubMesh.h"
#include "Runtime/Graphics/Scene/RenderWorld.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace HEngine
{
	namespace
	{
		class ScopedPreviewConfigState final
		{
		public:
			ScopedPreviewConfigState(uint32_t width, uint32_t height)
				: mPreviousViewportSize(ConfigManager::mViewportSize),
				  mPreviousSelectedEntity(ConfigManager::selectedEntity)
			{
				ConfigManager::mViewportSize = {
					static_cast<float>(width),
					static_cast<float>(height) };
				ConfigManager::selectedEntity = -1;
			}

			~ScopedPreviewConfigState() noexcept
			{
				ConfigManager::mViewportSize = mPreviousViewportSize;
				ConfigManager::selectedEntity = mPreviousSelectedEntity;
			}

		private:
			glm::vec2 mPreviousViewportSize;
			int mPreviousSelectedEntity = -1;
		};

		void ExpandBounds(
			glm::vec3& minimum,
			glm::vec3& maximum,
			const glm::mat4& transform,
			const glm::vec3& localMinimum,
			const glm::vec3& localMaximum)
		{
			const std::array<glm::vec3, 8> corners = {{
				{ localMinimum.x, localMinimum.y, localMinimum.z },
				{ localMaximum.x, localMinimum.y, localMinimum.z },
				{ localMinimum.x, localMaximum.y, localMinimum.z },
				{ localMaximum.x, localMaximum.y, localMinimum.z },
				{ localMinimum.x, localMinimum.y, localMaximum.z },
				{ localMaximum.x, localMinimum.y, localMaximum.z },
				{ localMinimum.x, localMaximum.y, localMaximum.z },
				{ localMaximum.x, localMaximum.y, localMaximum.z },
			}};
			for (const glm::vec3& corner : corners)
			{
				const glm::vec3 worldCorner = glm::vec3(transform * glm::vec4(corner, 1.0f));
				minimum = glm::min(minimum, worldCorner);
				maximum = glm::max(maximum, worldCorner);
			}
		}

		[[nodiscard]] Ref<const RenderWorld> AcquireMergedRenderWorld(Level& level)
		{
			Ref<const RenderWorld> world3D;
			Ref<const RenderWorld> world2D;
			if (RenderSystem3D* renderSystem = level.TryGetSystem<RenderSystem3D>())
				world3D = renderSystem->AcquireLatestRenderWorld();
			if (RenderSystem2D* renderSystem = level.TryGetSystem<RenderSystem2D>())
				world2D = renderSystem->AcquireLatestRenderWorld();
			if (!world3D)
				return world2D;
			if (!world2D || world2D->Primitives2D.empty())
				return world3D;

			Ref<RenderWorld> merged = CreateRef<RenderWorld>(*world3D);
			merged->Primitives2D.insert(
				merged->Primitives2D.end(),
				world2D->Primitives2D.begin(),
				world2D->Primitives2D.end());
			return merged;
		}
	}

	SceneThumbnailPreview::SceneThumbnailPreview(std::string debugName)
		: mViewport(std::move(debugName)),
		  mCamera(45.0f, 1.0f, 0.05f, 1000.0f)
	{
	}

	SceneThumbnailPreview::~SceneThumbnailPreview()
	{
		Release();
	}

	void SceneThumbnailPreview::SetScene(Ref<Level> scene)
	{
		if (!scene)
			throw std::invalid_argument("Scene thumbnail preview requires a scene");
		mScene = std::move(scene);
		mIs3D = mScene->TryGetSystem<RenderSystem3D>() != nullptr;
		ResetCamera();
		mError.clear();
	}

	void SceneThumbnailPreview::ClearScene() noexcept
	{
		mScene.reset();
		mError.clear();
	}

	bool SceneThumbnailPreview::Render(Timestep ts, uint32_t width, uint32_t height)
	{
		if (!mScene)
		{
			mError = "Scene thumbnail preview has no scene";
			return false;
		}

		width = std::max(width, 1u);
		height = std::max(height, 1u);
		mViewport.Activate(width, height);
		const ScopedPreviewConfigState previewConfig(width, height);
		mCamera.SetViewportSize(static_cast<float>(width), static_cast<float>(height));

		mPrimitiveCollector.BeginFrame();
		try
		{
			Renderer2D::CollectorScope collectorScope(mPrimitiveCollector);
			if (RenderSystem3D* renderSystem = mScene->TryGetSystem<RenderSystem3D>())
				renderSystem->OnUpdateEditor(ts, mCamera);
			if (RenderSystem2D* renderSystem = mScene->TryGetSystem<RenderSystem2D>())
				renderSystem->OnUpdateEditor(ts, mCamera);
			Ref<const RenderWorld> world = AcquireMergedRenderWorld(*mScene);
			if (!world)
				throw std::runtime_error("Scene thumbnail did not produce a RenderWorld");
			mViewport.Submit(std::move(world));
			mPrimitiveCollector.EndFrame();
		}
		catch (...)
		{
			mPrimitiveCollector.CancelFrame();
			throw;
		}
		return true;
	}

	TextureReadbackHandle SceneThumbnailPreview::RequestCapture(
		std::filesystem::path outputPath)
	{
		return mViewport.RequestCapture(std::move(outputPath));
	}

	void SceneThumbnailPreview::Release() noexcept
	{
		mScene.reset();
		mViewport.Release();
		mError.clear();
	}

	void SceneThumbnailPreview::ResetCamera()
	{
		glm::vec3 minimum(std::numeric_limits<float>::max());
		glm::vec3 maximum(std::numeric_limits<float>::lowest());
		bool bHasBounds = false;

		const auto meshView = mScene->mRegistry.view<TransformComponent, MeshComponent>();
		for (const EntityHandle handle : meshView)
		{
			const MeshComponent& meshComponent = meshView.get<MeshComponent>(handle);
			if (!meshComponent.mMesh)
				continue;
			glm::vec3 localMinimum(std::numeric_limits<float>::max());
			glm::vec3 localMaximum(std::numeric_limits<float>::lowest());
			bool bHasMeshBounds = false;
			for (const Ref<SubMesh>& subMesh : meshComponent.mMesh->mSubMeshes)
			{
				if (!subMesh || !subMesh->HasBounds())
					continue;
				localMinimum = glm::min(localMinimum, subMesh->GetBoundsMin());
				localMaximum = glm::max(localMaximum, subMesh->GetBoundsMax());
				bHasMeshBounds = true;
			}
			if (!bHasMeshBounds)
				continue;
			ExpandBounds(
				minimum,
				maximum,
				mScene->GetWorldTransform(Entity{ handle, mScene.get() }),
				localMinimum,
				localMaximum);
			bHasBounds = true;
		}

		auto expandUnitQuadView = [this, &minimum, &maximum, &bHasBounds](auto view)
		{
			for (const EntityHandle handle : view)
			{
				ExpandBounds(
					minimum,
					maximum,
					mScene->GetWorldTransform(Entity{ handle, mScene.get() }),
					glm::vec3(-0.5f, -0.5f, -0.01f),
					glm::vec3(0.5f, 0.5f, 0.01f));
				bHasBounds = true;
			}
		};
		expandUnitQuadView(mScene->mRegistry.view<TransformComponent, SpriteRendererComponent>());
		expandUnitQuadView(mScene->mRegistry.view<TransformComponent, CircleRendererComponent>());

		if (bHasBounds)
		{
			mPreviewCenter = (minimum + maximum) * 0.5f;
			mPreviewRadius = std::max(glm::length(maximum - minimum) * 0.5f, 0.5f);
		}
		else
		{
			mPreviewCenter = glm::vec3(0.0f);
			mPreviewRadius = 5.0f;
		}

		const float distance = std::max(mPreviewRadius * (mIs3D ? 2.5f : 2.0f), 2.5f);
		mCamera.SetNearPlane(std::max(0.01f, distance * 0.0005f));
		mCamera.SetFarPlane(std::max(1000.0f, distance + mPreviewRadius * 4.0f));
		mCamera.UpdateProjection();
		mCamera.SetCenter(mPreviewCenter);
		mCamera.SetDistance(distance);
		mCamera.SetOrientation(mIs3D
			? glm::quat(glm::vec3(glm::radians(-22.0f), glm::radians(-38.0f), 0.0f))
			: glm::quat(glm::vec3(0.0f)));
	}
}
