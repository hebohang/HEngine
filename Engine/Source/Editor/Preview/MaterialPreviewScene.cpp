#include "Editor/Preview/MaterialPreviewScene.h"

#include "Runtime/EcsFramework/Component/Light/DirectionalLightComponent.h"
#include "Runtime/EcsFramework/Component/Light/EnvironmentLightComponent.h"
#include "Runtime/EcsFramework/Component/Light/PointLightComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/EcsFramework/System/Render/RenderSystem3D.h"
#include "Runtime/Events/Event.h"
#include "Runtime/Events/MouseEvent.h"
#include "Runtime/Graphics/Material/Material.h"
#include "Runtime/Graphics/Mesh/Mesh.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
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

			ScopedPreviewConfigState(const ScopedPreviewConfigState&) = delete;
			ScopedPreviewConfigState& operator=(const ScopedPreviewConfigState&) = delete;

		private:
			glm::vec2 mPreviousViewportSize;
			int mPreviousSelectedEntity = -1;
		};

		class ScopedIntOverride final
		{
		public:
			ScopedIntOverride(int& value, int replacement)
				: mValue(value), mPrevious(value)
			{
				mValue = replacement;
			}

			~ScopedIntOverride() noexcept
			{
				mValue = mPrevious;
			}

			ScopedIntOverride(const ScopedIntOverride&) = delete;
			ScopedIntOverride& operator=(const ScopedIntOverride&) = delete;

		private:
			int& mValue;
			int mPrevious = 0;
		};

		[[nodiscard]] bool ComputeMeshBounds(
			const Mesh& mesh,
			glm::vec3& boundsMin,
			glm::vec3& boundsMax)
		{
			boundsMin = glm::vec3(std::numeric_limits<float>::max());
			boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
			bool bHasBounds = false;
			for (const Ref<SubMesh>& subMesh : mesh.mSubMeshes)
			{
				if (!subMesh || !subMesh->HasBounds())
					continue;
				boundsMin = glm::min(boundsMin, subMesh->GetBoundsMin());
				boundsMax = glm::max(boundsMax, subMesh->GetBoundsMax());
				bHasBounds = true;
			}
			return bHasBounds;
		}
	}

	MaterialPreviewScene::MaterialPreviewScene(std::string debugName, Ref<Material> material)
		: mMaterial(std::move(material)),
		  mViewport(std::move(debugName)),
		  mCamera(45.0f, 16.0f / 9.0f, 0.05f, 1000.0f)
	{
	}

	MaterialPreviewScene::~MaterialPreviewScene()
	{
		Release();
	}

	void MaterialPreviewScene::SetMaterial(Ref<Material> material)
	{
		if (!material)
			throw std::invalid_argument("Material preview requires a draft material");
		mMaterial = std::move(material);
		if (mScene)
			BindDraftMaterial();
	}

	bool MaterialPreviewScene::Render(
		Timestep ts,
		uint32_t width,
		uint32_t height,
		std::optional<double> materialTimeSeconds)
	{
		if (!EnsureScene())
			return false;

		width = std::max(width, 1u);
		height = std::max(height, 1u);
		mViewport.Activate(width, height);
		const ScopedPreviewConfigState previewConfig(width, height);

		mCamera.SetViewportSize(static_cast<float>(width), static_cast<float>(height));
		if (mViewportHovered)
			mCamera.OnUpdate(ts);

		mPrimitiveCollector.BeginFrame();
		try
		{
			Renderer2D::CollectorScope collectorScope(mPrimitiveCollector);
			if (materialTimeSeconds)
			{
				mRenderSystem->OnUpdateEditorAtMaterialTime(
					ts,
					mCamera,
					*materialTimeSeconds);
			}
			else
			{
				mRenderSystem->OnUpdateEditor(ts, mCamera);
			}
			Ref<const RenderWorld> previewWorld = mRenderSystem->AcquireLatestRenderWorld();
			if (!previewWorld)
				throw std::runtime_error("Material preview did not produce a RenderWorld");
			mViewport.Submit(std::move(previewWorld));
			mPrimitiveCollector.EndFrame();
		}
		catch (...)
		{
			mPrimitiveCollector.CancelFrame();
			throw;
		}
		return true;
	}

	TextureReadbackHandle MaterialPreviewScene::RequestCapture(
		std::filesystem::path outputPath)
	{
		return mViewport.RequestCapture(std::move(outputPath));
	}

	ImGuiTextureHandle MaterialPreviewScene::GetImGuiTexture()
	{
		return mViewport.GetImGuiTexture();
	}

	void MaterialPreviewScene::OnEvent(Event& event)
	{
		if (!mViewportHovered)
			return;

		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<MouseScrolledEvent>([this](MouseScrolledEvent& mouseEvent)
		{
			mCamera.OnEvent(mouseEvent);
			return true;
		});
		dispatcher.Dispatch<MouseButtonPressedEvent>([](MouseButtonPressedEvent&)
		{
			return true;
		});
		dispatcher.Dispatch<MouseButtonReleasedEvent>([](MouseButtonReleasedEvent&)
		{
			return true;
		});
	}

	void MaterialPreviewScene::ResetCamera()
	{
		const float distance = std::max(mPreviewRadius * 2.8f, 2.5f);
		mCamera.SetFarPlane(std::max(1000.0f, distance + mPreviewRadius * 4.0f));
		mCamera.UpdateProjection();
		mCamera.SetCenter(mPreviewCenter);
		mCamera.SetDistance(distance);
		mCamera.SetOrientation(glm::quat(glm::vec3(
			glm::radians(-18.0f),
			glm::radians(-32.0f),
			0.0f)));
		mCamera.mCameraSpeed = std::clamp(distance * 0.18f, 0.2f, 20.0f);
	}

	void MaterialPreviewScene::Release() noexcept
	{
		mViewportHovered = false;
		mViewport.Release();
		mPreviewEntity = {};
		mRenderSystem.reset();
		mScene.reset();
		mError.clear();
		mBuildFailed = false;
	}

	bool MaterialPreviewScene::EnsureScene()
	{
		if (mScene && mRenderSystem && mPreviewEntity)
			return true;
		if (mBuildFailed)
			return false;

		try
		{
			BuildScene();
			mError.clear();
			return true;
		}
		catch (const std::exception& exception)
		{
			mPreviewEntity = {};
			mRenderSystem.reset();
			mScene.reset();
			mError = exception.what();
			mBuildFailed = true;
			return false;
		}
	}

	void MaterialPreviewScene::BuildScene()
	{
		if (!mMaterial)
			throw std::invalid_argument("Material preview requires a draft material");

		Ref<Level> scene;
		{
			const ScopedIntOverride modeGuard(ModeManager::b3DMode, 1);
			scene = CreateRef<Level>();
		}
		Scope<RenderSystem3D> renderSystem = CreateScope<RenderSystem3D>(scene.get());

		Entity previewEntity = scene->CreateEntity("Material Preview Ball");
		auto& meshComponent = previewEntity.AddComponent<MeshComponent>(PreviewMeshPath);
		meshComponent.meshResPath = PreviewMeshPath;
		if (!meshComponent.mMesh || meshComponent.mMesh->mMaterial.size() < 2)
			throw std::runtime_error("Material preview mesh must expose draft slot 0 and neutral slot 1");

		bool bHasDraftSection = false;
		bool bHasNeutralSection = false;
		for (const Ref<SubMesh>& subMesh : meshComponent.mMesh->mSubMeshes)
		{
			if (!subMesh)
				continue;
			bHasDraftSection |= subMesh->mMaterialIndex == 0;
			bHasNeutralSection |= subMesh->mMaterialIndex == 1;
		}
		if (!bHasDraftSection || !bHasNeutralSection)
			throw std::runtime_error("Material preview mesh has an unexpected material-slot mapping");

		meshComponent.mMesh->mMaterial[0] = mMaterial;

		glm::vec3 boundsMin;
		glm::vec3 boundsMax;
		if (!ComputeMeshBounds(*meshComponent.mMesh, boundsMin, boundsMax))
			throw std::runtime_error("Material preview mesh has no valid bounds");
		mPreviewCenter = (boundsMin + boundsMax) * 0.5f;
		mPreviewRadius = std::max(glm::length(boundsMax - boundsMin) * 0.5f, 0.25f);

		Entity environmentLight = scene->CreateEntity("Material Preview Environment");
		auto& environment = environmentLight.AddComponent<EnvironmentLightComponent>();
		environment.HdrTexturePath = "Assets/Textures/Hdr/Default.hdr";
		environment.ExposureEV100 = 0.1520031f;
		environment.IBLIntensity = 1.15f;

		Entity keyLight = scene->CreateEntity("Material Preview Key Light");
		keyLight.GetComponent<TransformComponent>().Rotation =
			glm::radians(glm::vec3(48.0f, -35.0f, -20.0f));
		auto& directional = keyLight.AddComponent<DirectionalLightComponent>(
			2.6f,
			glm::vec3(1.0f, 0.96f, 0.88f));
		directional.bCastShadows = true;
		directional.ShadowMapSize = 1024;

		const float studioScale = std::max(mPreviewRadius, 1.0f);
		Entity fillLight = scene->CreateEntity("Material Preview Fill Light");
		fillLight.GetComponent<TransformComponent>().Translation =
			mPreviewCenter + glm::vec3(-2.0f, 1.5f, 2.0f) * studioScale;
		fillLight.AddComponent<PointLightComponent>(
			3.0f * studioScale * studioScale,
			glm::vec3(0.55f, 0.68f, 1.0f),
			6.0f * studioScale);

		mScene = std::move(scene);
		mRenderSystem = std::move(renderSystem);
		mPreviewEntity = previewEntity;
		ResetCamera();
	}

	void MaterialPreviewScene::BindDraftMaterial()
	{
		if (!mPreviewEntity || !mPreviewEntity.HasComponent<MeshComponent>())
			return;
		auto& meshComponent = mPreviewEntity.GetComponent<MeshComponent>();
		if (!meshComponent.mMesh || meshComponent.mMesh->mMaterial.empty())
			throw std::runtime_error("Material preview draft slot is unavailable");
		meshComponent.mMesh->mMaterial[0] = mMaterial;
	}
}
