#include "EditorLayer.h"
#include "EditorTheme.h"
#include "ImGuiWrapper/ImGuiWrapper.h"
#include "Manager/IconManager.h"
#include "Panels/BasePanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/OutputLogPanel.h"
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/MaterialPanel.h"
#include "Panels/AnimControllerPanel.h"
#include "Panels/AssetPreviewPanel.h"
#include "Panels/UIPanel.h"
#include "Panels/AudioPanel.h"
#include "Panels/BaseSettingsPanel.h"
#include "Panels/ProjectSettingsPanel.h"
#include "Panels/RenderGraphDiagnosticsPanel.h"
#include "Panels/OfflineRendererPanel.h"
#include "Preview/MaterialThumbnailService.h"
#include "Runtime/EcsFramework/Serializer/SceneSerializer.h"
#include "Runtime/EcsFramework/Component/Basic/TagComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Light/DirectionalLightComponent.h"
#include "Runtime/EcsFramework/Component/Light/PointLightComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/System/Embree/EmbreeSystem.h"
#include "Runtime/EcsFramework/System/Physics/PhysicSystem3D.h"
#include "Runtime/EcsFramework/System/Render/RenderSystem2D.h"
#include "Runtime/EcsFramework/System/Render/RenderSystem3D.h"
#include "Runtime/Graphics/Material/MaterialResourceCache.h"
#include "Runtime/Graphics/Material/MaterialShaderManager.h"
#include "Runtime/Graphics/Renderer/RuntimeRenderer.h"
#include "Runtime/Graphics/Settings/RenderSettingsService.h"
#include "Runtime/Utils/PlatformUtils.h"
#include "Runtime/Utils/IniUtils/IniFile.h"
#include "Runtime/Utils/MathUtils/MathUtils.h"
#include "Runtime/Utils/Misc/ShaderUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "Runtime/Utils/FileUtils/FileWatcher.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/RuntimeProjectResourceWarmup.h"
#include "Runtime/EcsFramework/System/UI/UIViewportContext.h"
#include "Runtime/UI/Rml/RmlUiManager.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/Core/Modules/ModuleManager.h"
#include "Runtime/Core/Plugins/Services/RenderCaptureService.h"
#include "Runtime/Core/Math/Math.h"

#include <HEngine/ProjectBuildConfig.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <ImGuizmo.h>
#include <ImGuizmoQuat/imGuIZMOquat.h>
#include <imgui-notify-v2/imgui_notify.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <magic_enum.hpp>
#include <algorithm>
#include <cmath>
#include <exception>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace HEngine
{
	namespace
	{
		constexpr const char* c_MaterialShowcaseScenePath =
			"Assets/Scenes/MaterialShowcase.he";

		void InitializeEditorCameraFromPrimary(
			EditorCamera& editorCamera,
			const Ref<Level>& scene)
		{
			if (!scene)
				return;

			auto cameras = scene->mRegistry.view<TransformComponent, CameraComponent>();
			for (const auto entity : cameras)
			{
				const auto& camera = cameras.get<CameraComponent>(entity);
				if (!camera.Primary ||
					camera.Camera.GetProjectionType() != SceneCamera::ProjectionType::Perspective)
				{
					continue;
				}

				glm::vec3 position;
				glm::vec3 rotation;
				glm::vec3 scale;
				if (!Math::DecomposeTransform(
						scene->GetWorldTransform(Entity(entity, scene.get())),
						position,
						rotation,
						scale))
				{
					continue;
				}

				const glm::quat orientation = glm::quat(rotation);
				const glm::vec3 forward = glm::normalize(
					glm::rotate(orientation, glm::vec3(0.0f, 0.0f, -1.0f)));
				constexpr float initialOrbitDistance = 10.0f;
				const glm::vec3 focalPoint =
					position + forward * initialOrbitDistance;

				editorCamera.mFOV = glm::degrees(
					camera.Camera.GetPerspectiveVerticalFOV());
				editorCamera.SetNearPlane(camera.Camera.GetPerspectiveNearClip());
				editorCamera.SetFarPlane(camera.Camera.GetPerspectiveFarClip());
				editorCamera.SetCenter(focalPoint);
				editorCamera.SetDistance(initialOrbitDistance);
				editorCamera.SetOrientation(orientation);
				editorCamera.UpdateProjection();
				return;
			}
		}

		void ReportEditorCleanupFailure(std::string_view operation, const std::exception_ptr& failure) noexcept
		{
			try
			{
				if (failure)
					std::rethrow_exception(failure);
			}
			catch (const std::exception& exception)
			{
				Log::WriteFallbackError(operation, exception.what());
			}
			catch (...)
			{
				Log::WriteFallbackError(operation, "unknown exception");
			}
		}

		void ReportEditorActionFailure(std::string_view operation, const std::exception_ptr& failure) noexcept
		{
			try
			{
				if (failure)
					std::rethrow_exception(failure);
			}
			catch (const std::exception& exception)
			{
				Log::WriteFallbackError(operation, exception.what());
				try
				{
					ImGui::InsertNotification({ ImGuiToastType_Error, 8000, "%.*s: %s",
						static_cast<int>(operation.size()), operation.data(), exception.what() });
				}
				catch (...)
				{
				}
			}
			catch (...)
			{
				Log::WriteFallbackError(operation, "unknown exception");
				try
				{
					ImGui::InsertNotification({ ImGuiToastType_Error, 8000, "%.*s",
						static_cast<int>(operation.size()), operation.data() });
				}
				catch (...)
				{
				}
			}
		}

		void ReleaseEditorRuntimeWarmupResidency(std::string_view operation) noexcept
		{
			try
			{
				RuntimeProjectResourceWarmup& projectWarmup =
					Application::GetInstance().GetProjectResourceWarmup();
				(void)projectWarmup.ReleaseResidency(RuntimeResourceWarmupResidency::Session);
				(void)projectWarmup.ReleaseResidency(RuntimeResourceWarmupResidency::Startup);
			}
			catch (...)
			{
				ReportEditorCleanupFailure(operation, std::current_exception());
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

		[[nodiscard]] std::string BuildMaterialPanelKey(const std::string& normalizedPath)
		{
			return "MaterialEditor:" + normalizedPath;
		}

		[[nodiscard]] RenderBackend GetPersistedGraphicsApiPreference()
		{
			RenderBackend backend = RenderBackend::DX12;
			RenderSettingsService& service =
				RenderSettingsService::GetInstance();
			for (const SettingsLayer layer : {
					SettingsLayer::EngineDefault,
					SettingsLayer::Project,
					SettingsLayer::User })
			{
				const Json values = service.GetLayerValues(layer);
				const auto value = values.find(
					std::string(RenderSettingIds::Backend));
				if (value != values.end() && value->is_string())
					backend = ParseRenderBackend(value->get<std::string>());
			}
			return backend;
		}

	}

	Scope<HIni> EditorLayer::mTextIni = nullptr;

	namespace Utils
	{
		// from https://github.com/Acmen-Team/Epoch/tree/dev
		template<typename UIFunction>
		static void SceneToolbar(ImGuiDockNode* node, const float DISTANCE, int* corner, UIFunction uiFunc)
		{
			ImGuiIO& io = ImGui::GetIO();
			ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;

			ImVec2 work_area_pos = node->Pos;
			ImVec2 work_area_size = node->Size;

			if (*corner != -1)
			{
				window_flags |= ImGuiWindowFlags_NoMove;
				ImGuiViewport* viewport = ImGui::GetMainViewport();

				ImVec2 window_pos = ImVec2((*corner & 1) ? (work_area_pos.x + work_area_size.x - DISTANCE) : (work_area_pos.x + DISTANCE), (*corner & 2) ? (work_area_pos.y + work_area_size.y - DISTANCE) : (work_area_pos.y + DISTANCE));
				ImVec2 window_pos_pivot = ImVec2((*corner & 1) ? 1.0f : 0.0f, (*corner & 2) ? 1.0f : 0.0f);
				ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, window_pos_pivot);
				ImGui::SetNextWindowViewport(node->ID);
			}
			ImGui::SetNextWindowBgAlpha(0.0f); // Transparent background

			uiFunc(corner, work_area_size, window_flags);
		}
	}

	// Setting
	static bool bShowBaseSettings = false;
	static bool bShowProjectSettings = false;
	static bool bShowRenderGraphDiagnostics = false;
	static bool bShowOfflineRenderer = false;

	// Window
	static bool bShowViewport = true;
	static bool bShowContentBrowser = true;
	static bool bShowSceneHierachy = true;
	static bool bShowProperties = true;
	static bool bShowStats = false;
	static bool bShowSRT = true;
	static bool bShowOutputLog = true;
	static bool bShowFPS = false;

	// Help
	static bool bShowTutorial = false;
	static bool bShowAboutMe = false;
	static bool bShowDemoImGui = false;

	std::unordered_map<std::string, Scope<BasePanel>> EditorLayer::mPanelMap;

		namespace
		{
			constexpr float kPointLightIconSizePx = 54.0f;
			constexpr float kDirectionalLightIconSizePx = 54.0f;
			constexpr float kCameraIconSizePx = 58.0f;
			constexpr float kSelectedIconScale = 1.12f;
			constexpr float kDirectionalArrowStartOffsetPx = 34.0f;
			constexpr float kDirectionalArrowLengthPx = 96.0f;
			constexpr float kDirectionalArrowHeadLengthPx = 24.0f;
			constexpr float kDirectionalArrowHeadWidthPx = 14.0f;

			static std::string DumpJson(const Json& json)
			{
				return json.dump();
			}

			static std::string MakeUndoTooltip(const char* actionLabel, const char* stepLabel)
			{
				std::string tooltip = actionLabel;
				if (stepLabel && stepLabel[0] != '\0')
				{
					tooltip += "\n";
					tooltip += stepLabel;
				}
				return tooltip;
			}

			static std::string GetSceneDisplayName(const std::filesystem::path& scenePath, const char* untitledLabel)
			{
				const std::string fallback = untitledLabel ? untitledLabel : "Untitled";
				if (scenePath.empty())
					return fallback;

				const std::string stem = scenePath.stem().string();
				return stem.empty() ? fallback : stem;
			}

			static std::string BuildSceneBadgeText(const std::filesystem::path& scenePath, bool bDirty, const char* untitledLabel)
			{
				std::string label = GetSceneDisplayName(scenePath, untitledLabel);
				if (bDirty)
					label += " *";
				return label;
			}

			static std::string BuildViewportSizeText(const glm::vec2& viewportSize)
			{
				const int width = viewportSize.x > 0.0f ? (int)viewportSize.x : 0;
				const int height = viewportSize.y > 0.0f ? (int)viewportSize.y : 0;
				return std::to_string(width) + " x " + std::to_string(height);
			}

			static ImGuiTextureHandle GetViewportMarkerIcon(const char* iconName)
			{
				IconManager& iconManager = IconManager::GetInstance();
				if (const ImGuiTextureHandle icon = iconManager.TryGet(iconName))
					return icon;
				return iconManager.TryGet("EntityIcon");
			}

			static ImU32 ToImGuiColor(const glm::vec4& color)
			{
				return ImGui::ColorConvertFloat4ToU32(ImVec4(color.r, color.g, color.b, color.a));
			}

			static float GetEditorViewDepth(const EditorCamera& camera, const glm::vec3& worldPosition)
			{
				const glm::vec3 toMarker = worldPosition - camera.GetPosition();
				return glm::dot(toMarker, camera.GetForwardDirection());
			}

			static float ComputeWorldUnitsPerPixel(const EditorCamera& camera, const glm::vec3& worldPosition)
			{
				const float viewDepth = GetEditorViewDepth(camera, worldPosition);
				if (viewDepth <= camera.GetNearPlane() || ConfigManager::mViewportSize.y <= 0.0f)
					return 0.0f;

				const float halfFovRadians = glm::radians(camera.GetFOV()) * 0.5f;
				return 2.0f * viewDepth * std::tan(halfFovRadians) / ConfigManager::mViewportSize.y;
			}

			static bool ProjectWorldToViewport(const EditorCamera& camera, const glm::vec3& worldPosition, const ImVec2& viewportMin, const ImVec2& viewportSize, ImVec2& outScreenPosition)
			{
				const glm::vec4 clipPosition = camera.GetViewProjection() * glm::vec4(worldPosition, 1.0f);
				if (clipPosition.w <= 0.0001f)
					return false;

				const glm::vec3 ndc = glm::vec3(clipPosition) / clipPosition.w;
				if (ndc.z < -1.0f || ndc.z > 1.0f)
					return false;

				outScreenPosition.x = viewportMin.x + (ndc.x * 0.5f + 0.5f) * viewportSize.x;
				outScreenPosition.y = viewportMin.y + (-ndc.y * 0.5f + 0.5f) * viewportSize.y;
				return true;
			}

			static glm::vec3 BuildReadableLightTint(const glm::vec3& color)
			{
				return glm::max(glm::clamp(color, glm::vec3(0.0f), glm::vec3(1.0f)), glm::vec3(0.35f));
			}

			static glm::vec3 ComputeDirectionalLightRayDirection(const glm::mat4& worldTransform)
			{
				glm::vec3 storedDirection = glm::vec3(worldTransform * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
				if (glm::dot(storedDirection, storedDirection) <= 0.000001f)
					return glm::vec3(0.0f, 1.0f, 0.0f);
				return glm::normalize(storedDirection);
			}

			static void DrawViewportIcon(ImDrawList* drawList, const EditorCamera& camera, const ImVec2& viewportMin, const ImVec2& viewportSize, const glm::vec3& worldPosition, ImGuiTextureHandle icon, float iconSizePx, const glm::vec4& tint)
			{
				if (!drawList || !icon)
					return;

				if (GetEditorViewDepth(camera, worldPosition) <= camera.GetNearPlane())
					return;

				ImVec2 screenPosition;
				if (!ProjectWorldToViewport(camera, worldPosition, viewportMin, viewportSize, screenPosition))
					return;

				const ImVec2 halfExtent(iconSizePx * 0.5f, iconSizePx * 0.5f);
				drawList->AddImage(
					IconManager::ToImTextureID(icon),
					ImVec2(screenPosition.x - halfExtent.x, screenPosition.y - halfExtent.y),
					ImVec2(screenPosition.x + halfExtent.x, screenPosition.y + halfExtent.y),
					ImVec2(0.0f, 0.0f),
					ImVec2(1.0f, 1.0f),
					ToImGuiColor(tint));
			}

			static void DrawDirectionalLightArrow(ImDrawList* drawList, const EditorCamera& camera, const ImVec2& viewportMin, const ImVec2& viewportSize, const glm::vec3& worldPosition, const glm::vec3& direction, const glm::vec4& color)
			{
				if (!drawList)
					return;

				const float worldUnitsPerPixel = ComputeWorldUnitsPerPixel(camera, worldPosition);
				if (worldUnitsPerPixel <= 0.0f)
					return;

				const float shaftLength = kDirectionalArrowLengthPx * worldUnitsPerPixel;

				const glm::vec3 arrowDirection = glm::normalize(direction);
				ImVec2 originScreenPosition;
				ImVec2 tipScreenPosition;
				if (!ProjectWorldToViewport(camera, worldPosition, viewportMin, viewportSize, originScreenPosition))
					return;
				if (!ProjectWorldToViewport(camera, worldPosition + arrowDirection * shaftLength, viewportMin, viewportSize, tipScreenPosition))
					return;

				ImVec2 screenDirection = ImVec2(tipScreenPosition.x - originScreenPosition.x, tipScreenPosition.y - originScreenPosition.y);
				const float screenDirectionLength = std::sqrt(screenDirection.x * screenDirection.x + screenDirection.y * screenDirection.y);
				if (screenDirectionLength <= 1.0f)
					return;

				screenDirection.x /= screenDirectionLength;
				screenDirection.y /= screenDirectionLength;

				const ImVec2 arrowStart(
					originScreenPosition.x + screenDirection.x * kDirectionalArrowStartOffsetPx,
					originScreenPosition.y + screenDirection.y * kDirectionalArrowStartOffsetPx);
				const ImVec2 arrowEnd(
					arrowStart.x + screenDirection.x * kDirectionalArrowLengthPx,
					arrowStart.y + screenDirection.y * kDirectionalArrowLengthPx);
				const ImVec2 arrowNormal(-screenDirection.y, screenDirection.x);
				const ImVec2 headBase(
					arrowEnd.x - screenDirection.x * kDirectionalArrowHeadLengthPx,
					arrowEnd.y - screenDirection.y * kDirectionalArrowHeadLengthPx);
				const ImU32 arrowColor = ToImGuiColor(color);

				drawList->AddLine(arrowStart, arrowEnd, arrowColor, 2.0f);
				drawList->AddLine(arrowEnd, ImVec2(headBase.x + arrowNormal.x * kDirectionalArrowHeadWidthPx, headBase.y + arrowNormal.y * kDirectionalArrowHeadWidthPx), arrowColor, 2.0f);
				drawList->AddLine(arrowEnd, ImVec2(headBase.x - arrowNormal.x * kDirectionalArrowHeadWidthPx, headBase.y - arrowNormal.y * kDirectionalArrowHeadWidthPx), arrowColor, 2.0f);
			}

			static void RenderEditorEntityMarkers(ImDrawList* drawList, const Ref<Level>& scene, const EditorCamera& camera, const ImVec2& viewportMin, const ImVec2& viewportSize, SceneHierarchyPanel& hierarchyPanel)
			{
				if (!drawList || !scene || !ModeManager::b3DMode || viewportSize.x <= 0.0f || viewportSize.y <= 0.0f)
					return;

				const Entity selectedEntity = hierarchyPanel.GetSelectedEntity();
				const ImGuiTextureHandle pointLightIcon = GetViewportMarkerIcon("PointLightEntityIcon");
				const ImGuiTextureHandle directionalLightIcon = GetViewportMarkerIcon("DirLightEntityIcon");
				const ImGuiTextureHandle cameraIcon = GetViewportMarkerIcon("CameraEntityIcon");

				drawList->PushClipRect(viewportMin, ImVec2(viewportMin.x + viewportSize.x, viewportMin.y + viewportSize.y), true);

				auto cameraView = scene->mRegistry.view<TransformComponent, CameraComponent>();
				for (auto entityHandle : cameraView)
				{
					Entity entity = { entityHandle, scene.get() };
					if (!hierarchyPanel.IsEntityVisible(entity))
						continue;

					const bool bSelected = entity == selectedEntity;
					const float iconSizePx = kCameraIconSizePx * (bSelected ? kSelectedIconScale : 1.0f);
					const float alpha = bSelected ? 1.0f : 0.94f;
					DrawViewportIcon(drawList, camera, viewportMin, viewportSize, scene->GetWorldTranslation(entity), cameraIcon, iconSizePx, glm::vec4(1.0f, 1.0f, 1.0f, alpha));
				}

				auto pointLightView = scene->mRegistry.view<TransformComponent, PointLightComponent>();
				for (auto entityHandle : pointLightView)
				{
					Entity entity = { entityHandle, scene.get() };
					if (!hierarchyPanel.IsEntityVisible(entity))
						continue;

					auto& light = pointLightView.get<PointLightComponent>(entityHandle);
					const bool bSelected = entity == selectedEntity;
					const float iconSizePx = kPointLightIconSizePx * (bSelected ? kSelectedIconScale : 1.0f);
					const float alpha = bSelected ? 1.0f : 0.94f;
					const glm::vec3 tint = BuildReadableLightTint(light.LightColor);
					DrawViewportIcon(drawList, camera, viewportMin, viewportSize, scene->GetWorldTranslation(entity), pointLightIcon, iconSizePx, glm::vec4(tint, alpha));
				}

				auto directionalLightView = scene->mRegistry.view<TransformComponent, DirectionalLightComponent>();
				for (auto entityHandle : directionalLightView)
				{
					Entity entity = { entityHandle, scene.get() };
					if (!hierarchyPanel.IsEntityVisible(entity))
						continue;

					auto& light = directionalLightView.get<DirectionalLightComponent>(entityHandle);
					const bool bSelected = entity == selectedEntity;
					const float iconSizePx = kDirectionalLightIconSizePx * (bSelected ? kSelectedIconScale : 1.0f);
					const float alpha = bSelected ? 1.0f : 0.94f;
					const glm::vec3 lightTint = BuildReadableLightTint(light.Color);
					const glm::vec3 worldPosition = scene->GetWorldTranslation(entity);
					const glm::mat4 worldTransform = scene->GetWorldTransform(entity);
					DrawViewportIcon(drawList, camera, viewportMin, viewportSize, worldPosition, directionalLightIcon, iconSizePx, glm::vec4(lightTint, alpha));
					DrawDirectionalLightArrow(drawList, camera, viewportMin, viewportSize, worldPosition, ComputeDirectionalLightRayDirection(worldTransform), glm::vec4(lightTint, 1.0f));
				}

				drawList->PopClipRect();
			}
		}

    EditorLayer::EditorLayer()
        : Layer("EditorLayer")
    {
		mActiveUndoClient = this;
		mLastActiveUndoClient = this;
		RegisterPanel<ContentBrowserPanel>("Content Browser", &bShowContentBrowser); // 为了保证默认进入Editor先看到内容浏览器而不是输出日志
		RegisterPanel<OutputLogPanel>("Output Log", &bShowOutputLog);

		EditorLogInit();
	}

	void EditorLayer::CreateMaterialPanel(std::string matPath)
	{
		if (matPath.empty())
		{
			HE_CORE_WARN("CreateMaterialPanel: empty material path, ignored.");
			return;
		}

		const std::string normalizedPath = Utils::NormalizeAssetPath(matPath);
		if (std::filesystem::path(normalizedPath).extension() != ".hmat")
		{
			HE_CORE_WARN("CreateMaterialPanel: invalid material extension '{0}'", matPath);
			return;
		}

		const std::filesystem::path fullPath = ConfigManager::GetFullPath(normalizedPath);
		if (!std::filesystem::exists(fullPath))
		{
			HE_CORE_WARN("CreateMaterialPanel: material does not exist '{0}'", fullPath.string());
			return;
		}

		const std::string panelKey = BuildMaterialPanelKey(normalizedPath);
		if (auto iter = mPanelMap.find(panelKey); iter != mPanelMap.end())
		{
			iter->second->Open();
			if (ImGui::GetCurrentContext())
				ImGui::SetWindowFocus(normalizedPath.c_str());
			return;
		}

		if (mIteratingPanelMap)
		{
			if (std::find(mPendingMaterialPaths.begin(), mPendingMaterialPaths.end(), normalizedPath) == mPendingMaterialPaths.end())
				mPendingMaterialPaths.push_back(normalizedPath);
			return;
		}

		mPanelMap[panelKey] = CreateScope<MaterialPanel>(this, normalizedPath);
	}

	void EditorLayer::PreviewMaterialAsset(const std::string& matPath, const Material& material)
	{
		if (matPath.empty())
			return;
		if (mMaterialThumbnailService)
			mMaterialThumbnailService->Invalidate(matPath);
		const std::string materialKey = Utils::MakeProjectPathCacheKey(matPath);
		bool bApplied = false;

		auto applyToScene = [&](const Ref<Level>& scene)
		{
			if (!scene)
				return;

			auto view = scene->mRegistry.view<MeshComponent>();
			for (auto entity : view)
			{
				auto& meshComponent = view.get<MeshComponent>(entity);
				if (!meshComponent.mMesh)
					continue;

				for (auto& targetMaterial : meshComponent.mMesh->mMaterial)
				{
					if (!targetMaterial)
						continue;
					if (Utils::MakeProjectPathCacheKey(targetMaterial->mMatResPath) !=
						materialKey)
						continue;

					targetMaterial->CopyRuntimeStateFrom(material);
					bApplied = true;
				}
			}
		};

		applyToScene(mEditorScene);
		if (mActiveScene != mEditorScene)
			applyToScene(mActiveScene);
		if (bApplied)
			++mEditorRenderRevision;
	}

	void EditorLayer::RefreshMaterialsForCompiledShader(
		const std::filesystem::path& shaderPath,
		bool bCompilationSucceeded,
		std::string_view compilationDiagnostics)
	{
		std::string normalizedShaderPath;
		try
		{
			const std::string relativePath = ConfigManager::GetRelativePath(shaderPath);
			if (!Utils::TryNormalizeProjectRelativePath(
					relativePath,
					normalizedShaderPath) ||
				!Utils::ToLowerCopy(normalizedShaderPath).starts_with("shaders/") ||
				Utils::ToLowerCopy(
					Utils::PathToUtf8(
						Utils::Utf8ToPath(normalizedShaderPath).extension())) != ".hlsl")
			{
				HE_CORE_WARN(
					"Ignoring material refresh for non-content shader path: {0}",
					shaderPath.string());
				return;
			}
		}
		catch (const std::exception& exception)
		{
			HE_CORE_WARN(
				"Could not normalize recompiled shader path '{0}': {1}",
				shaderPath.string(),
				exception.what());
			return;
		}

		if (!bCompilationSucceeded)
		{
			const std::string message = compilationDiagnostics.empty()
				? "Shader compilation failed; the last-known-good program remains active."
				: std::string(compilationDiagnostics) +
					" The last-known-good program remains active.";
			for (const auto& [panelKey, panel] : mPanelMap)
			{
				(void)panelKey;
				MaterialPanel* materialPanel =
					dynamic_cast<MaterialPanel*>(panel.get());
				if (materialPanel && materialPanel->UsesShader(normalizedShaderPath))
				{
					materialPanel->NotifyShaderReloadFailed(
						nullptr,
						0,
						message);
				}
			}
			return;
		}

		MaterialShaderRefreshResult programRefresh;
		try
		{
			programRefresh = Application::GetInstance()
				.RefreshMaterialShaderAtFrameBoundary(normalizedShaderPath);
		}
		catch (const std::exception& exception)
		{
			programRefresh.Diagnostics.push_back({
				.Severity = MaterialShaderDiagnosticSeverity::Error,
				.Code = "MaterialProgram.FrameBoundary",
				.Message = exception.what(),
				.Source = normalizedShaderPath,
			});
		}

		if (!programRefresh.Succeeded())
		{
			const std::string message = programRefresh.Diagnostics.empty()
				? "GPU program publication failed; the last-known-good program remains active."
				: FormatMaterialShaderDiagnostics(programRefresh.Diagnostics);
			for (const auto& [panelKey, panel] : mPanelMap)
			{
				(void)panelKey;
				MaterialPanel* materialPanel =
					dynamic_cast<MaterialPanel*>(panel.get());
				if (!materialPanel || !materialPanel->UsesShader(normalizedShaderPath))
					continue;
				materialPanel->NotifyShaderReloadFailed(
					programRefresh.Program
						? &programRefresh.Program->GetIdentity()
						: nullptr,
					programRefresh.Program
						? programRefresh.Program->GetGeneration()
						: 0,
					message);
			}
			HE_CORE_WARN(
				"Material shader '{}' kept its last-known-good program: {}",
				normalizedShaderPath,
				message);
			return;
		}

		const CompiledMaterialShader& publishedProgram = *programRefresh.Program;
		const MaterialShaderManifest& publishedManifest = publishedProgram.GetManifest();
		const MaterialShaderProgramIdentity& publishedIdentity =
			publishedProgram.GetIdentity();
		const uint64_t publishedGeneration = publishedProgram.GetGeneration();

		for (const auto& [panelKey, panel] : mPanelMap)
		{
			(void)panelKey;
			MaterialPanel* materialPanel =
				dynamic_cast<MaterialPanel*>(panel.get());
			if (!materialPanel || !materialPanel->UsesShader(normalizedShaderPath))
				continue;

			if (materialPanel->RefreshShaderLayoutFromPublishedProgram(
				publishedManifest,
				publishedIdentity,
				publishedGeneration))
			{
				if (mMaterialThumbnailService)
				{
					mMaterialThumbnailService->Invalidate(
						materialPanel->GetMaterialPath());
				}
			}
			else
			{
				HE_CORE_WARN(
					"Material panel kept its last-known-good layout after shader refresh: {0}",
					materialPanel->GetMaterialPath());
			}
		}

		MaterialResourceCache& materialCache =
			MaterialResourceCache::GetInstance();
		std::vector<Ref<Material>> liveMaterials;
		try
		{
			liveMaterials = materialCache.GetLiveMaterialsForShader(
				normalizedShaderPath);
		}
		catch (const std::exception& exception)
		{
			HE_CORE_WARN(
				"Could not query live materials for shader '{0}': {1}",
				normalizedShaderPath,
				exception.what());
			return;
		}
		liveMaterials.erase(
			std::remove_if(
				liveMaterials.begin(),
				liveMaterials.end(),
				[](const Ref<Material>& material)
				{
					return !material || !material->IsCustomSurfaceShader();
				}),
			liveMaterials.end());
		if (liveMaterials.empty())
			return;

		bool bLiveMaterialChanged = false;
		for (const Ref<Material>& material : liveMaterials)
		{
			if (!material)
				continue;
			MaterialShaderLayoutRefreshResult materialRefresh =
				material->RefreshShaderLayoutFromPublishedProgram(
					publishedManifest,
					publishedIdentity,
					publishedGeneration);
			if (!materialRefresh.bSucceeded)
			{
				HE_CORE_WARN(
					"Material '{}' kept its last-known-good layout after program publication: {}",
					material->mMatResPath,
					materialRefresh.Error);
				continue;
			}
			bLiveMaterialChanged = true;
			if (mMaterialThumbnailService && !material->mMatResPath.empty())
				mMaterialThumbnailService->Invalidate(material->mMatResPath);
		}
		if (bLiveMaterialChanged)
			++mEditorRenderRevision;
	}

	bool EditorLayer::IsMaterialPanelOpen(std::string_view matPath) const
	{
		const std::string normalized = Utils::NormalizeAssetPath(std::string(matPath));
		return !normalized.empty() &&
			mPanelMap.contains(BuildMaterialPanelKey(normalized));
	}

	void EditorLayer::CreateAssetPreviewPanel(std::string assetPath)
	{
		if (assetPath.empty())
		{
			HE_CORE_WARN("CreateAssetPreviewPanel: empty asset path, ignored.");
			return;
		}

		const std::string normalizedPath = Utils::NormalizeAssetPath(assetPath);
		const std::filesystem::path extPath(normalizedPath);
		const std::string extension = extPath.extension().string();
		if (extension != ".hmesh" && extension != ".hanim" && extension != ".hskel")
		{
			HE_CORE_WARN("CreateAssetPreviewPanel: unsupported preview extension '{0}'", assetPath);
			return;
		}

		const std::string panelKey = AssetPreviewPanel::BuildPanelKey(normalizedPath);
		if (auto iter = mPanelMap.find(panelKey); iter != mPanelMap.end())
		{
			iter->second->Open();
			return;
		}

		if (mIteratingPanelMap)
		{
			if (std::find(mPendingAssetPreviewPaths.begin(), mPendingAssetPreviewPaths.end(), normalizedPath) == mPendingAssetPreviewPaths.end())
				mPendingAssetPreviewPaths.push_back(normalizedPath);
			return;
		}

		mPanelMap[panelKey] = CreateScope<AssetPreviewPanel>(this, normalizedPath);
	}

	void EditorLayer::FlushPendingMaterialPanels()
	{
		if (mPendingMaterialPaths.empty())
			return;

		std::vector<std::string> pendingPaths;
		pendingPaths.swap(mPendingMaterialPaths);
		for (const std::string& materialPath : pendingPaths)
			CreateMaterialPanel(materialPath);
	}

	void EditorLayer::FlushPendingAssetPreviewPanels()
	{
		if (mPendingAssetPreviewPaths.empty())
			return;

		std::vector<std::string> pendingPaths;
		pendingPaths.swap(mPendingAssetPreviewPaths);
		for (const std::string& assetPath : pendingPaths)
			CreateAssetPreviewPanel(assetPath);
	}

	void EditorLayer::OpenAudioPanel(std::string audioPath)
	{
		if (mPanelMap.count(audioPath))
		{
			mPanelMap[audioPath]->Open();
		}
		else
		{
			mPanelMap[audioPath] = CreateScope<AudioPanel>(this, audioPath);
		}
	}

	void EditorLayer::CreateAnimControllerPanel(std::string controllerPath)
	{
		if (controllerPath.empty())
		{
			HE_CORE_WARN("CreateAnimControllerPanel: empty controller path, ignored.");
			return;
		}

		const std::filesystem::path extPath(controllerPath);
		if (extPath.extension() != ".hanimctrl")
		{
			HE_CORE_WARN("CreateAnimControllerPanel: invalid controller extension '{0}'", controllerPath);
			return;
		}

		const std::string normalizedPath = Utils::NormalizeAssetPath(controllerPath);
		const std::string panelKey = AnimControllerPanel::BuildPanelKey(normalizedPath);
		if (mPanelMap.count(panelKey))
		{
			mPanelMap[panelKey]->Open();
			return;
		}

		mPanelMap[panelKey] = CreateScope<AnimControllerPanel>(this, normalizedPath);
	}

	void EditorLayer::CreateUIPanel(std::string uiPath)
	{
		if (uiPath.empty())
		{
			HE_CORE_WARN("CreateUIPanel: empty ui path, ignored.");
			return;
		}

		const std::filesystem::path extPath(uiPath);
		if (extPath.extension() != ".hui")
		{
			HE_CORE_WARN("CreateUIPanel: invalid ui extension '{0}'", uiPath);
			return;
		}

		const std::string normalizedPath = Utils::NormalizeAssetPath(uiPath);
		const std::string panelKey = UIPanel::BuildPanelKey(normalizedPath);
		if (mPanelMap.count(panelKey))
		{
			mPanelMap[panelKey]->Open();
			return;
		}

		mPanelMap[panelKey] = CreateScope<UIPanel>(this, normalizedPath);
	}

	template<typename PanelType>
	void EditorLayer::RegisterPanel(std::string panelName, bool* pPanelOpen)
	{
		mPanelMap[PanelType::GetTypeName()] = CreateScope<PanelType>(this, panelName, pPanelOpen);
	}

	void EditorLayer::EditorLogInit()
	{
		using my_sink_st = my_sink<spdlog::details::null_mutex>;
		Log::GetCoreLogger() = CreateRef<spdlog::logger>("HEngine", CreateRef<my_sink_st>());
		Log::GetCoreLogger()->set_level(spdlog::level::trace);
	}

    void EditorLayer::OnAttach()
    {
		// UE-style editor panels stay inside one native window. This also avoids the
		// extra swap chains and focus ambiguity created by detached platform windows.
		ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;

		// register panel
		// panel name 必须和 ini 配置中的一致
		RegisterPanel<SceneHierarchyPanel>("Scene Hierarchy", &bShowSceneHierachy);
		RegisterPanel<PropertiesPanel>("Properties", &bShowProperties);
		RegisterPanel<BaseSettingsPanel>(
			"Editor Preferences",
			&bShowBaseSettings);
		RegisterPanel<ProjectSettingsPanel>(
			"Project Settings",
			&bShowProjectSettings);
		RegisterPanel<RenderGraphDiagnosticsPanel>(
			"RenderGraph Diagnostics",
			&bShowRenderGraphDiagnostics);
		RegisterPanel<OfflineRendererPanel>(
			"Offline Renderer",
			&bShowOfflineRenderer);

		// Layout fallback can be selected below before the scene is opened. Load its
		// panel defaults first so a fresh or differently sized automation layout is safe.
		mBaseEditorJson = Utils::LoadJsonFile(
			ConfigManager::GetEngineContentFullPath("Config/BaseEditor.json"));
		const auto projectEditorConfigPath = ConfigManager::GetProjectFullPath("Config/BaseEditor.json");
		if (std::filesystem::is_regular_file(projectEditorConfigPath))
			mBaseEditorJson.merge_patch(Utils::LoadJsonFile(projectEditorConfigPath));
		const bool bStartFullscreen =
			mBaseEditorJson.contains("Fullscreen") &&
			mBaseEditorJson["Fullscreen"].is_boolean() &&
			mBaseEditorJson["Fullscreen"].get<bool>();
		Application::GetWindow().SetFullscreen(bStartFullscreen);

		ImGuiContext& g = *GImGui;
		const ApplicationRunOptions& runOptions = Application::GetInstance().GetRunOptions();
		const std::filesystem::path editorLayoutPath =
			ConfigManager::GetSavedPath("EditorLayout.ini");

		// PushLayer invokes OnAttach synchronously before Application::Run starts its first
		// frame, so the automation layout path is installed before ImGui can load settings.
		// ImGui keeps this pointer until context shutdown; process-lifetime storage is required.
		static std::string sEditorLayoutPath;
		sEditorLayoutPath = editorLayoutPath.string();
		g.IO.IniFilename = sEditorLayoutPath.c_str();

		if (!std::filesystem::exists(editorLayoutPath))
		{
			const std::filesystem::path DefaultEditorConfigPath{ ConfigManager::GetFullPath("Config/EditorLayoutDefault.ini") };
			HE_CORE_ASSERT(std::filesystem::exists(DefaultEditorConfigPath));
			ImGui::LoadIniSettingsFromDisk(DefaultEditorConfigPath.string().c_str());
		}

		const float currentWindowWidth = (float)Application::GetWindow().GetWidth();
		if (ImGuiWindowSettings* dockSpaceSettings = ImGui::FindWindowSettingsByID(ImHashStr("DockSpace Demo")))
		{
			const float savedDockWidth = (float)dockSpaceSettings->Size.x;
			if (currentWindowWidth > 0.0f && savedDockWidth > 0.0f)
			{
				const float widthScaleRatio = savedDockWidth / currentWindowWidth;
				if (widthScaleRatio > 1.35f || widthScaleRatio < 0.74f)
				{
					HE_CORE_INFO("Editor layout resolution changed (saved {:.1f} vs current {:.1f}), loading default layout.", savedDockWidth, currentWindowWidth);
					LoadDefaultEditorConfig();
				}
			}
		}

		EmbreeSystem::InitializeSharedContext();
		Application::GetInstance().EnableMainOffscreenOutput();
		mMaterialThumbnailService = CreateScope<MaterialThumbnailService>();
		mSceneThumbnailService = CreateScope<SceneThumbnailService>();
		mActiveScene = CreateRef<Level>();

		mEditorCamera = EditorCamera(30.0f, 1.778f, 0.1f, 10000.0f);

		// Default Scene, with an explicit automation override that never mutates
		// the project or engine editor configuration.
		const std::string startupScene = runOptions.StartupScene.empty()
			? mBaseEditorJson["DefaultScene"].get<std::string>()
			: runOptions.StartupScene;
		OpenScene(ConfigManager::GetFullPath(startupScene));
		ResetSceneHistory("Open Scene");

		if (ConfigManager::GetLanguage() == Language::English)
			mTextIni = CreateScope<HIni>(ConfigManager::GetEditorResourceFullPath("Localization/English.ini").string().c_str());
		else if (ConfigManager::GetLanguage() == Language::Chinese)
			mTextIni = CreateScope<HIni>(ConfigManager::GetEditorResourceFullPath("Localization/Chinese.ini").string().c_str());

		shaderWatcher.AddFile(ConfigManager::GetInstance().GetShadersFolder());
		const auto engineShaderRoot = ConfigManager::GetEngineContentFullPath("Shaders");
		if (engineShaderRoot != ConfigManager::GetInstance().GetShadersFolder())
			shaderWatcher.AddFile(engineShaderRoot);
		if (mShaderWatcherHandle)
			shaderWatcher.GetFileModifiedEvent().Remove(mShaderWatcherHandle);
		mShaderWatcherHandle = shaderWatcher.GetFileModifiedEvent().Add([this](const std::filesystem::path& filePath)
		{
			const std::string extension = Utils::ToLowerCopy(filePath.extension().string());
			if (extension == ".hlsl")
			{
				// DXIL content hashes are part of modern shader cache keys, so the
				// next explicit pipeline request receives the recompiled variant.
				std::string diagnostics;
				const bool bSucceeded = Utils::CompileShader(
					filePath,
					&diagnostics);
				RefreshMaterialsForCompiledShader(
					filePath,
					bSucceeded,
					diagnostics);
				return;
			}

			if (extension != ".hlsli")
				return;

			for (const Utils::ShaderCompilationResult& result :
				Utils::CompileDependentShadersForInclude(filePath))
			{
				if (!result.bSucceeded && !result.Diagnostics.empty())
				{
					HE_CORE_WARN(
						"Dependent shader compilation failed for '{0}': {1}",
						result.ShaderPath.string(),
						result.Diagnostics);
				}
				RefreshMaterialsForCompiledShader(
					result.ShaderPath,
					result.bSucceeded,
					result.Diagnostics);
			}
		});
    }

	void EditorLayer::OnPreUpdate()
	{
		HE_PROFILE_SECTION("Shader Watcher");
		shaderWatcher.Update();
	}

	void EditorLayer::OnDetach()
	{
		if (mShaderWatcherHandle)
			shaderWatcher.GetFileModifiedEvent().Remove(mShaderWatcherHandle);
		if (mViewportModeController.IsOfflineRendererActive())
		{
			try
			{
				Application::GetInstance().DeactivateOfflineViewportRenderer();
				(void)mViewportModeController.PrepareForPlay();
			}
			catch (...)
			{
				ReportEditorCleanupFailure(
					"Offline viewport renderer shutdown failed during detach.",
					std::current_exception());
			}
		}
		if (!ModeManager::IsEditState())
		{
			try
			{
				OnSceneStop();
			}
			catch (...)
			{
				ReportEditorCleanupFailure("Editor runtime stop failed during detach.", std::current_exception());
			}
		}
		mResetEditorDockLayout = ImGui::FindWindowSettingsByID(ImHashStr("##HEditorLayoutV2")) == nullptr;
		ReleaseEditorRuntimeWarmupResidency(
			"Editor resource warm-up release failed during detach.");

		try
		{
			mRuntimeAltDown = false;
			SetRuntimeCursorCaptured(false);
			Application::GetWindow().SetInputMethodEnabled(true);
		}
		catch (...)
		{
			ReportEditorCleanupFailure("Editor input restoration failed during detach.", std::current_exception());
		}

		try
		{
			GetPanel<OutputLogPanel>()->ShutDown();
		}
		catch (...)
		{
			ReportEditorCleanupFailure("Editor output log shutdown failed during detach.", std::current_exception());
		}
		mSceneThumbnailService.reset();
		mMaterialThumbnailService.reset();
		mPanelMap.clear();
		mPendingMaterialPaths.clear();
		mPendingAssetPreviewPaths.clear();
		mSceneHistoryEntries.clear();
		mSceneHistoryCursor = 0;
		mSceneHistoryInteractivePending = false;
		mPendingSceneHistoryLabel.clear();
		mSceneDirty = false;
		mActiveScene.reset();
		mEditorScene.reset();
		if (mMainViewportTexture)
		{
			Application::GetInstance().UnregisterImGuiTexture(mMainViewportTexture);
			mMainViewportTexture = {};
			mMainViewportOutputGeneration = 0;
		}
		IconManager::GetInstance().Shutdown();
		EmbreeSystem::ShutdownSharedContext();
	}

    void EditorLayer::OnUpdate(Timestep ts)
    {
        HE_PROFILE;

		UpdateRuntimeCursorPolicy();

		if (ModeManager::IsEditState())
		{
			HE_PROFILE_SECTION("Editor Update");
			if (bViewportHovered)
				mEditorCamera.OnUpdate(ts);

			const glm::mat4 currentView = mEditorCamera.GetViewMatrix();
			const glm::mat4 currentProjection = mEditorCamera.GetProjection();
			const bool bCameraChanged =
				std::memcmp(glm::value_ptr(currentView), glm::value_ptr(mLastRenderedEditorView), sizeof(glm::mat4)) != 0 ||
				std::memcmp(glm::value_ptr(currentProjection), glm::value_ptr(mLastRenderedEditorProjection), sizeof(glm::mat4)) != 0;
			const bool bEditorStateChanged =
				mLastRenderedEditorRevision != mEditorRenderRevision ||
				mLastRenderedSelectedEntity != ConfigManager::selectedEntity ||
				mLastRenderedDimensionMode != ModeManager::b3DMode ||
				mLastRenderedShowColliders != ModeManager::bShowPhysicsColliders ||
				mLastRenderedPhysicsDebugMode != ModeManager::mPhysicsDebugDrawModeFlag;
			const bool bUpdateEditorScene =
				mEditorWarmupFramesRemaining > 0 ||
				bCameraChanged ||
				bEditorStateChanged ||
				mViewportModeController.IsOfflineRendererActive() ||
				ModeManager::bRealtimePreview ||
				mActiveScene->RequiresContinuousEditorUpdate();

			if (bUpdateEditorScene)
			{
				// Time-driven materials only need a fresh RenderWorld. Their shader
				// time comes from PresentationClock during extraction, so keep the
				// editor timestep paused unless the user enabled realtime preview.
				const Timestep editorTimestep = ModeManager::bRealtimePreview ? ts : Timestep(0.0f);
				mActiveScene->OnUpdateEditor(editorTimestep, mEditorCamera);
				Application::GetInstance().SubmitRenderWorld(AcquireMergedRenderWorld(*mActiveScene));
				if (mEditorWarmupFramesRemaining > 0)
					--mEditorWarmupFramesRemaining;
				mLastRenderedEditorView = currentView;
				mLastRenderedEditorProjection = currentProjection;
				mLastRenderedEditorRevision = mEditorRenderRevision;
				mLastRenderedSelectedEntity = ConfigManager::selectedEntity;
				mLastRenderedDimensionMode = ModeManager::b3DMode;
				mLastRenderedShowColliders = ModeManager::bShowPhysicsColliders;
				mLastRenderedPhysicsDebugMode = ModeManager::mPhysicsDebugDrawModeFlag;
			}
		}
		else
		{
			HE_PROFILE_SECTION("Runtime Update");
			const float viewportWidth = ConfigManager::mViewportSize.x;
			const float viewportHeight = ConfigManager::mViewportSize.y;
			const float uiScale = Application::GetInstance().GetImGuiLayer() ? Application::GetInstance().GetImGuiLayer()->GetUIScale() : 1.0f;
			const ImVec2 mainViewportPosition = ImGui::GetMainViewport()->Pos;
			const glm::vec2 rmlViewportOrigin = {
				mViewportBounds[0].x - mainViewportPosition.x,
				mViewportBounds[0].y - mainViewportPosition.y
			};
			{
				HE_PROFILE_SECTION("UI Viewport Context");
				UIViewportContext::Set(mViewportBounds[0].x, mViewportBounds[0].y, viewportWidth, viewportHeight, uiScale);
			}
			auto& rmlUi = RmlUiManager::GetInstance();
			{
				HE_PROFILE_SECTION("RmlUi Viewport Rect");
				rmlUi.SetViewportRect(rmlViewportOrigin.x, rmlViewportOrigin.y, viewportWidth, viewportHeight, uiScale);
			}
			{
				HE_PROFILE_SECTION("RmlUi Begin Frame");
				const RuntimeOffscreenOutput viewportOutput =
					Application::GetInstance().GetMainOffscreenOutput();
				rmlUi.BeginFrame(
					viewportWidth,
					viewportHeight,
					uiScale,
					viewportOutput ? static_cast<float>(viewportOutput.Width) : viewportWidth,
					viewportOutput ? static_cast<float>(viewportOutput.Height) : viewportHeight);
			}
			{
				HE_PROFILE_SECTION("Scene Runtime Update");
				mActiveScene->OnUpdateRuntime(ts);
			}
			Application::GetInstance().SubmitRenderWorld(AcquireMergedRenderWorld(*mActiveScene));
			UpdateRuntimeCursorPolicy();
			{
				HE_PROFILE_SECTION("RmlUi Update");
				rmlUi.Update();
			}
		}

		Entity selectedEntity = GetPanel<SceneHierarchyPanel>()->GetSelectedEntity();
		if (selectedEntity)
		{
			const bool bCanUseSelectedEntity = GetPanel<SceneHierarchyPanel>()->CanSelectEntity(selectedEntity);

			if (bCanUseSelectedEntity)
			{
				ConfigManager::selectedEntity = (int)(uint32_t)selectedEntity;
			}
			else
			{
				ConfigManager::selectedEntity = -1;
			}

			if (bCanUseSelectedEntity && Input::IsKeyPressed(HE_KEY_F))
				mEditorCamera.SetCenter(selectedEntity.GetLevel()->GetWorldTranslation(selectedEntity));
		}
		else
		{
			ConfigManager::selectedEntity = -1;
		}

		if (mMaterialThumbnailService)
		{
			HE_PROFILE_SECTION("Material Thumbnails");
			mMaterialThumbnailService->Tick(ts);
		}
		if (mSceneThumbnailService)
		{
			HE_PROFILE_SECTION("Scene Thumbnails");
			mSceneThumbnailService->Tick(ts);
		}
    }

	void EditorLayer::OnImGuiRender()
	{
		HE_PROFILE;

		ImGuiIO& uiIo = ImGui::GetIO();
		const bool bDisableEditorMouse = !ModeManager::IsEditState() && mRuntimeCursorCaptured;
		if (bDisableEditorMouse)
			uiIo.ConfigFlags |= ImGuiConfigFlags_NoMouse;
		else
			uiIo.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

		UpdateSettings();
		mFocusedUndoClient = nullptr;
		mHoveredUndoClient = nullptr;

		// Check async package build result
		if (mPackageFuture.valid() &&
			mPackageFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			auto [success, msg] = mPackageFuture.get();
			if (success)
			{
				HE_CORE_INFO("[Package] Windows 打包完成: {}", msg);
				ImGui::InsertNotification({ ImGuiToastType_Success, 5000, "Windows打包完成: %s", msg.c_str() });
				FileDialogs::OpenInExplorer(msg);
			}
			else
			{
				HE_CORE_ERROR("[Package] 打包失败: {}", msg);
				ImGui::InsertNotification({ ImGuiToastType_Error, 8000, "打包失败: %s", msg.c_str() });
			}
		}

		// ----DockSpace Begin----
		static bool dockspaceOpen = true;
		static bool opt_fullscreen = true;
		static bool opt_padding = false;
		static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_PassthruCentralNode;
		const float statusBarHeight = ImGuiWrapper::S(30.0f);

        // We are using the ImGuiWindowFlags_NoDocking flag to make the parent window not dockable into,
        // because it would be confusing to have two docking targets within each others.
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
		if (opt_fullscreen)
		{
			const ImGuiViewport* viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(viewport->WorkPos);
			ImVec2 dockspaceSize = viewport->WorkSize;
			dockspaceSize.y = ImMax(1.0f, dockspaceSize.y - statusBarHeight);
			ImGui::SetNextWindowSize(dockspaceSize);
			ImGui::SetNextWindowViewport(viewport->ID);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
            window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        }
        else
        {
            dockspace_flags &= ~ImGuiDockNodeFlags_PassthruCentralNode;
        }

        // When using ImGuiDockNodeFlags_PassthruCentralNode, DockSpace() will render our background
        // and handle the pass-thru hole, so we ask Begin() to not render a background.
        if (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode)
            window_flags |= ImGuiWindowFlags_NoBackground;

        // Important: note that we proceed even if Begin() returns false (aka window is collapsed).
        // This is because we want to keep our DockSpace() active. If a DockSpace() is inactive,
        // all active windows docked into it will lose their parent and become undocked.
        // We cannot preserve the docking relationship between an active window and an inactive docking, otherwise
        // any change of dockspace/settings would lead to windows being stuck in limbo and never being visible.
        if (!opt_padding)
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("DockSpace Demo", &dockspaceOpen, window_flags);
        if (!opt_padding)
            ImGui::PopStyleVar();

        if (opt_fullscreen)
            ImGui::PopStyleVar(2);

        // Submit the DockSpace
        ImGuiIO& io = ImGui::GetIO();
		ImGuiStyle& style = ImGui::GetStyle();
        float minWinSizeX = style.WindowMinSize.x;
        style.WindowMinSize.x = 110.0f;
        if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable)
        {
            ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
			if (mResetEditorDockLayout)
			{
				// Older layouts may contain orphaned dock spaces from localized window IDs.
				// Clear every stale node before constructing the single-window layout.
				ImGui::DockContextClearNodes(ImGui::GetCurrentContext(), 0, true);
				ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
				ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetContentRegionAvail());

				ImGuiID centerDock = dockspace_id;
				const ImGuiID rightDock = ImGui::DockBuilderSplitNode(centerDock, ImGuiDir_Right, 0.27f, nullptr, &centerDock);
				ImGuiID hierarchyDock = rightDock;
				const ImGuiID propertiesDock = ImGui::DockBuilderSplitNode(hierarchyDock, ImGuiDir_Down, 0.64f, nullptr, &hierarchyDock);
				const ImGuiID bottomDock = ImGui::DockBuilderSplitNode(centerDock, ImGuiDir_Down, 0.30f, nullptr, &centerDock);
				const ImGuiID toolbarDock = ImGui::DockBuilderSplitNode(centerDock, ImGuiDir_Up, 0.07f, nullptr, &centerDock);

				ImGui::DockBuilderDockWindow("##toolbar", toolbarDock);
				ImGui::DockBuilderDockWindow("Viewport", centerDock);
				ImGui::DockBuilderDockWindow("Content Browser", bottomDock);
				ImGui::DockBuilderDockWindow("Output Log", bottomDock);
				ImGui::DockBuilderDockWindow("Scene Hierarchy", hierarchyDock);
				ImGui::DockBuilderDockWindow("Properties", propertiesDock);
				ImGui::DockBuilderFinish(dockspace_id);
				if (ImGui::FindWindowSettingsByID(ImHashStr("##HEditorLayoutV2")) == nullptr)
					ImGui::CreateNewWindowSettings("##HEditorLayoutV2");
				mResetEditorDockLayout = false;
			}
            ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
        }

        style.WindowMinSize.x = minWinSizeX;

		// ----MenuBar Begin----
		if (ImGui::BeginMenuBar())
		{
            if (ImGui::BeginMenu(GetText("File")))
            {
				ImGui::BeginDisabled(!ModeManager::IsEditState());
                if (ImGui::MenuItem(GetText("New"), "Ctrl+N"))
                    NewScene();

                if (ImGui::MenuItem(GetText("Open..."), "Ctrl+O"))
                    OpenScene();

				if (ImGui::MenuItem(GetText("Save"), "Ctrl+S"))
					SaveScene();

                if (ImGui::MenuItem(GetText("Save As..."), "Ctrl+Shift+S"))
                    SaveSceneAs();
				ImGui::EndDisabled();

                if (ImGui::MenuItem(GetText("Exit"), NULL, false))
					Application::GetInstance().Close();

				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu(GetText("Edit")))
			{
				const bool bCanUndo = mActiveUndoClient && mActiveUndoClient->CanUndo();
				const bool bCanRedo = mActiveUndoClient && mActiveUndoClient->CanRedo();
				if (!bCanUndo)
					ImGui::BeginDisabled();
				if (ImGui::MenuItem(GetText("Undo"), "Ctrl+Z"))
					UndoActiveDocument();
				if (!bCanUndo)
					ImGui::EndDisabled();

				if (!bCanRedo)
					ImGui::BeginDisabled();
				if (ImGui::MenuItem(GetText("Redo"), "Ctrl+Y / Ctrl+Shift+Z"))
					RedoActiveDocument();
				if (!bCanRedo)
					ImGui::EndDisabled();

				ImGui::Separator();
				ImGui::BeginDisabled(!ModeManager::IsEditState());
				if (ImGui::MenuItem(GetText("Duplicate Entity"), "Ctrl+D"))
					OnDuplicateEntity();
				ImGui::EndDisabled();

				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu(GetText("Build")))
			{
				ImGui::BeginDisabled(!ModeManager::IsEditState());
				if (ImGui::MenuItem(GetText("Package Windows..."), "Ctrl+P"))
					PackageProjectWindows();
				ImGui::EndDisabled();
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu(GetText("Settings")))
			{
				if (ImGui::BeginMenu(GetText("Language")))
				{
					if (ImGui::MenuItem(GetText("English"), NULL, (ConfigManager::GetLanguage() == Language::English)))
					{
						ConfigManager::GetInstance().SetLanguage(Language::English);
						mTextIni = CreateScope<HIni>(ConfigManager::GetEditorResourceFullPath("Localization/English.ini").string().c_str());

						mBaseEditorJson["Language"] = "English";
						SaveBaseEditorConfig();
					}
					if (ImGui::MenuItem(GetText("Chinese"), NULL, (ConfigManager::GetLanguage() == Language::Chinese)))
					{
						ConfigManager::GetInstance().SetLanguage(Language::Chinese);
						mTextIni = CreateScope<HIni>(ConfigManager::GetEditorResourceFullPath("Localization/Chinese.ini").string().c_str());

						mBaseEditorJson["Language"] = "Chinese";
						SaveBaseEditorConfig();
					}
					ImGui::EndMenu();
				}

				ImGui::Separator();
				UI_GraphicsApiSettings();
				ImGui::Separator();
				ImGui::MenuItem(GetText("Editor Preferences"), NULL,
					&bShowBaseSettings);
				ImGui::MenuItem(GetText("Project Settings"), NULL,
					&bShowProjectSettings);
				ImGui::Separator();
				if (ImGui::BeginMenu(GetText("Rendering Tools")))
				{
					ImGui::MenuItem(GetText("RenderGraph Diagnostics"),
						nullptr, &bShowRenderGraphDiagnostics);
					if (!Application::GetInstance()
							.EnumerateOfflineViewportRenderers()
							.empty())
					{
						ImGui::MenuItem(
							"Offline Renderer",
							nullptr,
							&bShowOfflineRenderer);
					}
					ImGui::EndMenu();
				}

				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu(GetText("Window")))
			{
				ImGuiWrapper::SeparatorWithText(GetText("SeparatorPANEL"));

				ImGui::MenuItem(GetText("Viewport"), NULL, &bShowViewport);
				ImGui::MenuItem(GetText("Content Browser"), NULL, &bShowContentBrowser);
				ImGui::MenuItem(GetText("Level Hierachy"), NULL, &bShowSceneHierachy);
				ImGui::MenuItem(GetText("Properties"), NULL, &bShowProperties);
				ImGui::MenuItem(GetText("Stats"), NULL, &bShowStats);
				ImGui::MenuItem(EditorLayer::GetText("FPS Overlay"), NULL, &bShowFPS);

				ImGuiWrapper::SeparatorWithText(GetText("SeparatorLOG"));

				ImGui::MenuItem(GetText("Output Log"), NULL, &bShowOutputLog);

				ImGuiWrapper::SeparatorWithText(GetText("SeparatorLAYOUT"));

				if (ImGui::MenuItem(GetText("Save As Default Layout")))
				{
					SaveDefaultEditorConfig();
				}

				if (ImGui::MenuItem(GetText("Load Default Layout")))
				{
					LoadDefaultEditorConfig();
					mPanelMap.clear();
					mActiveUndoClient = this;
					mFocusedUndoClient = nullptr;
					mHoveredUndoClient = nullptr;
					mLastActiveUndoClient = this;
					RegisterPanel<ContentBrowserPanel>("Content Browser", &bShowContentBrowser);
					RegisterPanel<OutputLogPanel>("Output Log", &bShowOutputLog);
					RegisterPanel<SceneHierarchyPanel>("Scene Hierarchy", &bShowSceneHierachy);
					RegisterPanel<PropertiesPanel>("Properties", &bShowProperties);
					RegisterPanel<BaseSettingsPanel>(
						"Editor Preferences",
						&bShowBaseSettings);
					RegisterPanel<ProjectSettingsPanel>(
						"Project Settings",
						&bShowProjectSettings);
					RegisterPanel<RenderGraphDiagnosticsPanel>(
						"RenderGraph Diagnostics",
						&bShowRenderGraphDiagnostics);
					RegisterPanel<OfflineRendererPanel>(
						"Offline Renderer",
						&bShowOfflineRenderer);
					ImGui::EndMenu();
					ImGui::EndMenuBar();
					ImGui::End();
					return;
				}

				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu(GetText("Help")))
			{
				if (ImGui::MenuItem(GetText("Material Showcase")))
				{
					OpenScene(ConfigManager::GetEngineContentFullPath(
						c_MaterialShowcaseScenePath));
				}
				ImGui::Separator();
				ImGui::MenuItem(GetText("Tutorial"), NULL, &bShowTutorial);
				ImGui::MenuItem(GetText("About Me"), NULL, &bShowAboutMe);
				ImGui::MenuItem(GetText("Demo ImGui"), NULL, &bShowDemoImGui);

				ImGui::EndMenu();
			}

            ImGui::EndMenuBar();
        }
		// ----MenuBar End----
		UI_GraphicsApiRestartPopup();

		// ----Windows Begin----
		HE_PROFILE_SECTION("Panels");
		mIteratingPanelMap = true;
		for (const auto& [panelTypeName, panel] : mPanelMap)
		{
			HE_PROFILE_TRANSIENT_SECTION(panelTypeName.c_str());
			panel->OnImGuiRender();
		}
		mIteratingPanelMap = false;
		FlushPendingMaterialPanels();
		FlushPendingAssetPreviewPanels();
		ResolveActiveUndoClient();
		if (!ImGuizmo::IsUsing() && mSceneHistoryInteractivePending)
			CommitPendingSceneHistory();
		if (bShowStats)
		{
			const std::string statsWindowName = std::string(EditorLayer::GetText("Stats")) + "###Stats";
			ImGui::Begin(statsWindowName.c_str(), &bShowStats);

			float fps = ImGui::GetIO().Framerate;
			ImGui::Text("%s: %.1f (%.3f ms/frame)", EditorLayer::GetText("Editor FPS"), fps, 1000.0f / fps);
			ImGui::Text("%s: %ux%u", EditorLayer::GetText("Viewport"),
				Application::GetInstance().GetRenderWidth(),
				Application::GetInstance().GetRenderHeight());

			ImGui::End();
		}
		if (bShowViewport)
		{
			bViewGizmoHovered = false;
			bViewGizmoActive = false;

			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 0, 0 });
			const std::string viewportWindowName = std::string(EditorLayer::GetText("Viewport")) + "###Viewport";
			ImGui::Begin(
				viewportWindowName.c_str(),
				nullptr,
				ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
			NotifyUndoClientActivity(
				this,
				ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows),
				ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows));

			auto viewportMinRegion = ImGui::GetWindowContentRegionMin();
			auto viewportMaxRegion = ImGui::GetWindowContentRegionMax();
			auto viewportOffset = ImGui::GetWindowPos();
			mViewportBounds[0] = { viewportMinRegion.x + viewportOffset.x, viewportMinRegion.y + viewportOffset.y };
			mViewportBounds[1] = { viewportMaxRegion.x + viewportOffset.x, viewportMaxRegion.y + viewportOffset.y };

			bViewportHovered = ImGui::IsWindowHovered();
			bHandleEvent = ImGui::IsWindowFocused() || bViewportHovered;
			const bool bRenderTransformGizmo = ModeManager::IsEditState() && (bViewportHovered || ImGuizmo::IsUsing());
			const bool bRenderViewGizmo = ModeManager::IsEditState();

			ImVec2 viewportPanelSize = ImGui::GetContentRegionAvail();

			if ((ConfigManager::mViewportSize.x != viewportPanelSize.x) || (ConfigManager::mViewportSize.y != viewportPanelSize.y))
			{
				ConfigManager::mViewportSize = { viewportPanelSize.x, viewportPanelSize.y };
				OnViewportResize();
			}

			const RuntimeOffscreenOutput viewportOutput =
				Application::GetInstance().GetMainOffscreenOutput();
			if (viewportOutput)
			{
				if (!mMainViewportTexture ||
					mMainViewportOutputGeneration != viewportOutput.Generation)
				{
					if (mMainViewportTexture)
						Application::GetInstance().UnregisterImGuiTexture(mMainViewportTexture);
					mMainViewportTexture = Application::GetInstance().RegisterImGuiTexture(
						viewportOutput.Color,
						viewportOutput.ColorFormat);
					mMainViewportOutputGeneration = viewportOutput.Generation;
				}
				ImGui::Image(
					NvrhiImGuiRenderer::ToImTextureID(mMainViewportTexture),
					ImVec2{ ConfigManager::mViewportSize.x, ConfigManager::mViewportSize.y });
			}
			else
			{
				ImGui::Dummy(ImVec2{
					ConfigManager::mViewportSize.x,
					ConfigManager::mViewportSize.y });
			}
			const ImVec2 cursorAfterViewport = ImGui::GetCursorPos();
			ImGui::SetCursorScreenPos(ImVec2(mViewportBounds[0].x + ImGuiWrapper::S(12.0f), mViewportBounds[0].y + ImGuiWrapper::S(12.0f)));
			ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ImGuiWrapper::S(5.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, ImGuiWrapper::S(1.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGuiWrapper::S(6.0f, 4.0f));
			ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.075f, 0.085f, 0.10f, 0.94f));
			ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.24f, 0.28f, 0.34f, 0.92f));
			if (ImGui::BeginChild("##ViewportControls", ImGuiWrapper::S(330.0f, 36.0f), true,
				ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
			{
				if (ImGui::Button(EditorLayer::GetText("Show"), ImGuiWrapper::S(64.0f, 0.0f)))
					ImGui::OpenPopup("ViewportShowMenu");
				if (ImGui::BeginPopup("ViewportShowMenu"))
				{
					ImGui::MenuItem(EditorLayer::GetText("Realtime Preview"), nullptr, &ModeManager::bRealtimePreview);
					ImGui::Separator();
					ImGui::MenuItem(EditorLayer::GetText("Collision"), nullptr, &ModeManager::bShowPhysicsColliders);
					if (ImGui::BeginMenu(EditorLayer::GetText("Collision Draw Mode")))
					{
						constexpr auto flags = magic_enum::enum_values<PhysicsDebugDrawModeFlag>();
						for (auto flag : flags)
						{
							if (flag == PhysicsDebugDrawModeFlag::None)
								continue;
							const bool bEnabled = (ModeManager::mPhysicsDebugDrawModeFlag & flag) != PhysicsDebugDrawModeFlag::None;
							const std::string flagName(magic_enum::enum_name(flag));
							if (ImGui::MenuItem(EditorLayer::GetText(flagName.c_str()), nullptr, bEnabled))
							{
								if (bEnabled)
									ModeManager::mPhysicsDebugDrawModeFlag &= ~flag;
								else
									ModeManager::mPhysicsDebugDrawModeFlag |= flag;
								PhysicSystem3D::SetDebugMode(ModeManager::mPhysicsDebugDrawModeFlag);
							}
						}
						ImGui::EndMenu();
					}
					ImGui::EndPopup();
				}

				ImGui::SameLine(0.0f, ImGuiWrapper::S(10.0f));
				ImGui::TextDisabled("%s", EditorLayer::GetText("Camera Speed"));
				ImGui::SameLine(0.0f, ImGuiWrapper::S(6.0f));
				ImGui::SetNextItemWidth(ImGuiWrapper::S(140.0f));
				ImGui::SliderFloat("##ViewportCameraSpeed", &mEditorCamera.mCameraSpeed, 0.1f, 100.0f, "%.1fx", ImGuiSliderFlags_Logarithmic);
			}
			ImGui::EndChild();
			ImGui::PopStyleColor(2);
			ImGui::PopStyleVar(3);
			ImGui::SetCursorPos(cursorAfterViewport);
			ImGui::Dummy(ImVec2(0.0f, 0.0f));
			if (ModeManager::IsEditState() && mActiveScene)
			{
				RenderEditorEntityMarkers(
					ImGui::GetWindowDrawList(),
					mActiveScene,
					mEditorCamera,
					ImVec2(mViewportBounds[0].x, mViewportBounds[0].y),
					ImVec2(ConfigManager::mViewportSize.x, ConfigManager::mViewportSize.y),
					*GetPanel<SceneHierarchyPanel>());
			}

			const ImRect viewportDropRect(
				ImVec2(mViewportBounds[0].x, mViewportBounds[0].y),
				ImVec2(mViewportBounds[1].x, mViewportBounds[1].y));
			if (ModeManager::IsEditState() && ImGui::BeginDragDropTargetCustom(
				viewportDropRect,
				ImGui::GetID("ViewportContentDropTarget")))
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
				{
					const wchar_t* path = (const wchar_t*)payload->Data;
					const std::string scenePath = Utils::NormalizeAssetPath(
						(std::filesystem::path("Assets") / path).generic_string());
					OpenScene(ConfigManager::GetFullPath(scenePath));
				}
				ImGui::EndDragDropTarget();
			}

			// Gizmos
			Entity selectedEntity = GetPanel<SceneHierarchyPanel>()->GetSelectedEntity();
			const bool bSelectedEntityLocked = GetPanel<SceneHierarchyPanel>()->IsSelectedEntityLocked();
			if (selectedEntity && !GetPanel<SceneHierarchyPanel>()->IsEntityVisible(selectedEntity))
				selectedEntity = {};
			if (bRenderTransformGizmo && selectedEntity && !bSelectedEntityLocked && mGizmoType != -1)
			{
				ImGuizmo::SetOrthographic(!mToolbarPerspectiveView);
				ImGuizmo::SetDrawlist();
				ImGuizmo::SetGizmoSizeClipSpace(0.18f);

				ImGuizmo::SetRect(mViewportBounds[0].x, mViewportBounds[0].y, mViewportBounds[1].x - mViewportBounds[0].x, mViewportBounds[1].y - mViewportBounds[0].y);

				// Camera
				// Runtime camera from entity
				/*auto cameraEntity = mActiveScene->GetPrimaryCameraEntity();
				const auto& camera = cameraEntity.GetComponent<CameraComponent>().Camera;
				const glm::mat4& cameraProjection = camera.GetProjection();
				glm::mat4 cameraView = glm::inverse(cameraEntity.GetComponent<TransformComponent>().GetTransform());*/

				// Editor camera
				const glm::mat4& cameraProjection = mEditorCamera.GetProjection();
				glm::mat4 cameraView = mEditorCamera.GetViewMatrix();

				// Entity transform
				glm::mat4 transform = selectedEntity.GetLevel()->GetWorldTransform(selectedEntity);

				// Snapping
				bool snap = mToolbarGridSnap || Input::IsKeyPressed(Key::LeftControl);
				float snapValue = 0.5f; // Snap to 0.5m for translation/scale
				// Snap to 45 degrees for rotation
				if (mGizmoType == ImGuizmo::OPERATION::ROTATE)
					snapValue = 45.0f;

				float snapValues[3] = { snapValue, snapValue, snapValue };

				ImGuizmo::Manipulate(glm::value_ptr(cameraView), glm::value_ptr(cameraProjection),
					(ImGuizmo::OPERATION)mGizmoType, mToolbarUseLocalSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD, glm::value_ptr(transform),
					nullptr, snap ? snapValues : nullptr);

				if (ImGuizmo::IsUsing())
				{
					selectedEntity.GetLevel()->SetWorldTransform(selectedEntity, transform);
					MarkSceneDirty("Transform Entity", true);
				}
			}

			if (bRenderViewGizmo)
			{
				ImGuiWrapper::InternalDraw("#GizmoAxis", &bShowViewport, 0.0, WindowCorner::LeftDown, [this]() {
					const glm::quat orientation = mEditorCamera.GetOrientation();
					quat qRot(orientation.w, orientation.x, orientation.y, orientation.z);
					const float viewGizmoSize = ImGuiWrapper::S(56.0f);
					if (ImGui::gizmo3D("##gizmo1", qRot, viewGizmoSize, imguiGizmo::mode3Axes | imguiGizmo::cubeAtOrigin, true))
						mEditorCamera.SetOrientation(glm::quat(qRot.w, qRot.x, qRot.y, qRot.z));

					bViewGizmoHovered = ImGui::IsItemHovered();
					bViewGizmoActive = ImGui::IsItemActive();
					});
			}

			if (bShowFPS)
			{
				ImGuiWrapper::InternalDraw("##FPSOverlay", &bShowFPS, 10.0, WindowCorner::RightDown, []() {
					float fps = ImGui::GetIO().Framerate;
					ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "%s: %.1f", EditorLayer::GetText("Editor FPS"), fps);
					});
			}

			ImGui::End();
			ImGui::PopStyleVar();
		}
		// ----Windows End----

		// ----Help Begin----
		// TODO
		ImGuiWindowFlags helpMenuFlags = ImGuiWindowFlags_NoDocking;
		if (bShowTutorial)
		{
			const std::string tutorialWindowName = std::string(EditorLayer::GetText("Tutorial")) + "###Tutorial";
			ImGui::Begin(tutorialWindowName.c_str(), &bShowTutorial, helpMenuFlags);
			for (int line = 0; line < 10; ++line)
				ImGui::TextUnformatted(EditorLayer::GetText("Hello!"));
			ImGui::End();
		}
		if (bShowAboutMe)
		{
			const std::string aboutWindowName = std::string(EditorLayer::GetText("About Me")) + "###About Me";
			ImGui::Begin(aboutWindowName.c_str(), &bShowAboutMe, helpMenuFlags);
			ImGui::TextUnformatted(EditorLayer::GetText("My name is hbh!"));
			ImGui::End();
		}
		if (bShowDemoImGui)
		{
			ImGui::ShowDemoWindow(&bShowDemoImGui);
		}
		// ----Help End----

		UI_Toolbar();
		UI_StatusBar();

		Application::GetInstance().GetImGuiLayer()->BlockEvents(false);

        ImGui::End(); 
		// ----DockSpace End----

		// imgui notify
		if (mActiveScene)
		{
			if (ModeManager::IsEditState())
				mActiveScene->OnImGuiRenderEditor();
			else
			{
				UIViewportContext::Set(
					mViewportBounds[0].x, mViewportBounds[0].y,
					mViewportBounds[1].x - mViewportBounds[0].x,
					mViewportBounds[1].y - mViewportBounds[0].y,
					ImGuiWrapper::GetUIScale());
				mActiveScene->OnImGuiRender();
			}
		}

		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ImGuiWrapper::S(5.0f)); // Round borders
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(43.f / 255.f, 43.f / 255.f, 43.f / 255.f, 100.f / 255.f)); // Background color
		ImGui::RenderNotifications(); // <-- Here we render all notifications
		ImGui::PopStyleVar(1); // Don't forget to Pop()
		ImGui::PopStyleColor(1);

		if (mChangeDimensionPending)
		{
			mActiveScene->ChangeDimMode();
			mChangeDimensionPending = false;
		}
    }

	void EditorLayer::UI_GraphicsApiSettings()
	{
		if (!ImGui::BeginMenu(GetText("GraphicsAPI")))
			return;

		RenderSettingsService& service = RenderSettingsService::GetInstance();
		const ResolvedRenderSettings snapshot = service.GetSnapshot();
		const RenderBackend activeBackend =
			Application::GetInstance().GetRenderBackend();
		const RenderBackend preferredBackend =
			GetPersistedGraphicsApiPreference();

		ImGui::TextDisabled("%s: %s",
			GetText("Current Graphics API"), ToString(activeBackend));
		ImGui::TextDisabled("%s: %s",
			GetText("Next Startup Graphics API"),
			ToString(preferredBackend));
		ImGui::Separator();

		for (const RenderBackend backend : {
				RenderBackend::DX12,
				RenderBackend::Vulkan })
		{
			const RenderSettingAvailability availability =
				GetRenderBackendAvailability(backend, snapshot.Capabilities);
			const char* label = backend == RenderBackend::DX12
				? GetText("DirectX 12 (Default)")
				: GetText("Vulkan");
			ImGui::BeginDisabled(!availability.bAvailable);
			const bool bSelected = backend == preferredBackend;
			if (ImGui::MenuItem(label, nullptr, bSelected) &&
				availability.bAvailable && !bSelected)
			{
				SetPreferredGraphicsApi(backend);
			}
			if (!availability.bAvailable && ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", availability.Reason.c_str());
			ImGui::EndDisabled();
		}

		ImGui::Separator();
		ImGui::TextDisabled("%s", GetText(
			"Graphics API changes take effect after restarting the Editor."));
		ImGui::EndMenu();
	}

	void EditorLayer::SetPreferredGraphicsApi(RenderBackend backend)
	{
		mGraphicsApiTargetBackend = backend;
		mGraphicsApiActiveBackend =
			Application::GetInstance().GetRenderBackend();
		mGraphicsApiStatusMessage.clear();
		mGraphicsApiChangeSucceeded = false;

		RenderSettingsService& service = RenderSettingsService::GetInstance();
		const ResolvedRenderSettings snapshot = service.GetSnapshot();
		const RenderSettingAvailability availability =
			GetRenderBackendAvailability(backend, snapshot.Capabilities);
		if (!availability.bAvailable)
		{
			mGraphicsApiStatusMessage = availability.Reason;
		}
		else
		{
			RenderSettingsDraft draft =
				service.CreateDraft(SettingsLayer::User);
			draft.Patch[std::string(RenderSettingIds::Backend)] =
				ToString(backend);
			const RenderSettingsApplyResult result =
				service.ApplyDraft(draft, true);
			mGraphicsApiChangeSucceeded = result.bSuccess;
			if (!result.bSuccess)
			{
				mGraphicsApiStatusMessage = result.ErrorCode + " · " +
					result.Message;
			}
		}

		if (mGraphicsApiChangeSucceeded)
		{
			HE_CORE_INFO(
				"[Editor] Graphics API preference saved: {0}; active backend remains {1} until restart",
				ToString(mGraphicsApiTargetBackend),
				ToString(mGraphicsApiActiveBackend));
		}
		else
		{
			HE_CORE_ERROR("[Editor] Failed to save Graphics API preference: {0}",
				mGraphicsApiStatusMessage);
		}
		mOpenGraphicsApiStatusPopup = true;
	}

	void EditorLayer::UI_GraphicsApiRestartPopup()
	{
		const std::string popupName =
			std::string(GetText("Graphics API Setting")) +
			"###GraphicsApiSettingStatus";
		if (mOpenGraphicsApiStatusPopup)
		{
			ImGui::OpenPopup(popupName.c_str());
			mOpenGraphicsApiStatusPopup = false;
		}

		if (!ImGui::BeginPopupModal(popupName.c_str(), nullptr,
				ImGuiWindowFlags_AlwaysAutoResize))
		{
			return;
		}

		ImGui::PushTextWrapPos(
			ImGui::GetCursorPosX() + ImGuiWrapper::S(440.0f));
		if (mGraphicsApiChangeSucceeded)
		{
			ImGui::TextWrapped(GetText("Graphics API Restart Notice"),
				ToString(mGraphicsApiTargetBackend),
				ToString(mGraphicsApiActiveBackend));
		}
		else
		{
			ImGui::TextWrapped(GetText("Graphics API Save Failed"),
				mGraphicsApiStatusMessage.c_str());
		}
		ImGui::PopTextWrapPos();
		ImGui::Spacing();
		if (!mGraphicsApiChangeSucceeded)
		{
			if (ImGui::Button(GetText("OK"), ImGuiWrapper::S(96.0f, 0.0f)))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		if (ImGui::Button(GetText("Restart Later"),
				ImGuiWrapper::S(112.0f, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
		}
		ImGuiWrapper::SameLine(0.0f);
		const bool bCanRestartNow =
			!mSceneDirty && ModeManager::IsEditState();
		ImGui::BeginDisabled(!bCanRestartNow);
		if (ImGui::Button(GetText("Restart Now"),
				ImGuiWrapper::S(112.0f, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
			Application::GetInstance().RequestRestart();
		}
		ImGui::EndDisabled();
		if (!bCanRestartNow)
		{
			ImGui::Spacing();
			ImGui::TextDisabled("%s", mSceneDirty
				? GetText("Save the scene before restarting the Editor.")
				: GetText("Stop Play Mode before restarting the Editor."));
		}
		ImGui::EndPopup();
	}

	void EditorLayer::UI_Toolbar()
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGuiWrapper::S(6.0f, 6.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImGuiWrapper::S(4.0f, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImGuiWrapper::S(6.0f, 6.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGuiWrapper::S(6.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ImGuiWrapper::S(6.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.28f, 0.52f, 0.86f, 0.24f));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.10f, 0.11f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.20f, 0.23f, 0.28f, 0.80f));

		ImGui::Begin("##toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		if (ImGuiDockNode* node = ImGui::GetWindowDockNode())
			node->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;

		ImGui::BeginChild("##Toolbar", ImVec2(0.0f, ImGuiWrapper::S(46.0f)), true,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		IconManager& iconManager = IconManager::GetInstance();
		const std::vector<ViewportMode> viewportModes =
			mViewportModeController.EnumerateModes(
				Application::GetInstance().GetRenderCapabilities());
		const ViewportMode& currentViewportMode =
			mViewportModeController.GetCurrentMode();
		ImGui::SetNextItemWidth(ImGuiWrapper::S(118.0f));
		ImGui::BeginDisabled(!ModeManager::IsEditState());
		if (ImGui::BeginCombo(
				"##ViewportRendererMode",
				currentViewportMode.DisplayName.c_str()))
		{
			for (const ViewportMode& mode : viewportModes)
			{
				ImGui::BeginDisabled(!mode.bAvailable);
				const bool bSelected = mode.Id == currentViewportMode.Id;
				if (ImGui::Selectable(mode.DisplayName.c_str(), bSelected) &&
					mode.bAvailable)
				{
					ApplyViewportMode(mode.Id);
				}
				if (bSelected)
					ImGui::SetItemDefaultFocus();
				ImGui::EndDisabled();
				if (!mode.bAvailable && ImGui::IsItemHovered(
						ImGuiHoveredFlags_AllowWhenDisabled))
				{
					ImGui::SetTooltip("%s", mode.UnavailableReason.c_str());
				}
			}
			ImGui::EndCombo();
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip(
				"%s",
				mViewportModeError.empty()
					? EditorLayer::GetText("Viewport renderer mode")
					: mViewportModeError.c_str());
		}
		ImGui::SameLine();
		ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
		ImGui::SameLine();
		const bool bCanUndo = mActiveUndoClient && mActiveUndoClient->CanUndo();
		const bool bCanRedo = mActiveUndoClient && mActiveUndoClient->CanRedo();
		const char* undoStepLabel = bCanUndo ? mActiveUndoClient->GetUndoContextName() : nullptr;
		const char* redoStepLabel = bCanRedo ? mActiveUndoClient->GetUndoContextName() : nullptr;
		const ImVec2 toolbarIconSize = ImGuiWrapper::S(20.0f, 20.0f);
		auto drawToolbarButton = [&](const char* iconName, const char* tooltip, const ImVec4& tint, bool* outPressed = nullptr)
		{
			ImGuiTextureHandle icon = iconManager.TryGet(iconName);
			if (!icon)
				icon = iconManager.GetFileIcon();

			ImGui::PushID(iconName);
			const bool pressed = ImGui::ImageButton(
				"##ToolbarIcon",
				IconManager::ToImTextureID(icon),
				toolbarIconSize,
				ImVec2(0, 0),
				ImVec2(1, 1),
				ImVec4(0, 0, 0, 0),
				tint);
			ImGui::PopID();

			if (outPressed)
				*outPressed = pressed;

			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", tooltip);
		};

		bool bSavePressed = false;
		drawToolbarButton("SaveIcon", EditorLayer::GetText("Save Scene (Ctrl+S)"), ImVec4(1, 1, 1, 1), &bSavePressed);
		if (bSavePressed)
			SaveScene();

		ImGui::SameLine();
		ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
		ImGui::SameLine();

		if (!bCanUndo)
			ImGui::BeginDisabled();
		bool bUndoPressed = false;
		const std::string undoTooltip = MakeUndoTooltip(EditorLayer::GetText("Undo (Ctrl+Z)"), undoStepLabel);
		drawToolbarButton("UndoIcon", undoTooltip.c_str(), bCanUndo ? ImVec4(1, 1, 1, 1) : ImVec4(0.7f, 0.7f, 0.7f, 0.8f), &bUndoPressed);
		if (!bCanUndo)
			ImGui::EndDisabled();
		if (bUndoPressed)
			UndoActiveDocument();

		ImGui::SameLine();
		if (!bCanRedo)
			ImGui::BeginDisabled();
		bool bRedoPressed = false;
		const std::string redoTooltip = MakeUndoTooltip(EditorLayer::GetText("Redo (Ctrl+Y / Ctrl+Shift+Z)"), redoStepLabel);
		drawToolbarButton("RedoIcon", redoTooltip.c_str(), bCanRedo ? ImVec4(1, 1, 1, 1) : ImVec4(0.7f, 0.7f, 0.7f, 0.8f), &bRedoPressed);
		if (!bCanRedo)
			ImGui::EndDisabled();
		if (bRedoPressed)
			RedoActiveDocument();

		ImGui::SameLine();
		ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
		ImGui::SameLine();

		auto drawGizmoButton = [&](const char* iconName, int gizmoOp, const char* tooltip)
		{
			const bool bActive = mGizmoType == gizmoOp;
			if (bActive)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.55f, 0.85f, 0.40f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.55f, 0.85f, 0.60f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.55f, 0.85f, 0.75f));
			}

			bool bPressed = false;
			drawToolbarButton(iconName, tooltip, ImVec4(1, 1, 1, 1), &bPressed);
			if (bPressed)
				mGizmoType = gizmoOp;

			if (bActive)
				ImGui::PopStyleColor(3);
		};

		drawGizmoButton("TransIcon", ImGuizmo::TRANSLATE, EditorLayer::GetText("Translate (W)"));
		ImGui::SameLine();
		drawGizmoButton("RotateIcon", ImGuizmo::ROTATE, EditorLayer::GetText("Rotate (E)"));
		ImGui::SameLine();
		drawGizmoButton("ScaleIcon", ImGuizmo::SCALE, EditorLayer::GetText("Scale (R)"));

		ImGui::SameLine();
		ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
		ImGui::SameLine();

		{
			const ImVec4 snapTint = mToolbarGridSnap ? ImVec4(1, 1, 1, 1) : ImVec4(0.55f, 0.55f, 0.55f, 0.80f);
			bool bPressed = false;
			drawToolbarButton("GridSnapIcon", EditorLayer::GetText("Grid Snap"), snapTint, &bPressed);
			if (bPressed)
				mToolbarGridSnap = !mToolbarGridSnap;
		}

		ImGui::SameLine();
		{
			const ImVec4 localTint = mToolbarUseLocalSpace ? ImVec4(1, 1, 1, 1) : ImVec4(0.55f, 0.55f, 0.55f, 0.80f);
			bool bPressed = false;
			drawToolbarButton("LocalSpaceIcon", EditorLayer::GetText("Local/World Space"), localTint, &bPressed);
			if (bPressed)
				mToolbarUseLocalSpace = !mToolbarUseLocalSpace;
		}

		ImGui::SameLine();
		{
			const ImVec4 perspTint = mToolbarPerspectiveView ? ImVec4(1, 1, 1, 1) : ImVec4(0.55f, 0.55f, 0.55f, 0.80f);
			bool bPressed = false;
			drawToolbarButton("PerspectiveIcon", EditorLayer::GetText("Perspective/Orthographic"), perspTint, &bPressed);
			if (bPressed)
				mToolbarPerspectiveView = !mToolbarPerspectiveView;
		}

		if (mToolbarPerspectiveView)
			ImGuizmo::SetOrthographic(false);
		else
			ImGuizmo::SetOrthographic(true);

		const char* untitledLabel = EditorLayer::GetText("Untitled");
		const std::string sceneLabel = BuildSceneBadgeText(mEditorScenePath, mSceneDirty, untitledLabel);
		const char* modeLabel = ModeManager::IsEditState() ? EditorLayer::GetText("Edit Mode") : EditorLayer::GetText("Play Mode");
		const float chipSpacing = ImGuiWrapper::S(8.0f);
		const float scenePillWidth = ImGui::CalcTextSize(sceneLabel.c_str()).x + ImGuiWrapper::S(20.0f);
		const float modePillWidth = ImGui::CalcTextSize(modeLabel).x + ImGuiWrapper::S(20.0f);
		const float playButtonWidth = toolbarIconSize.x + ImGui::GetStyle().FramePadding.x * 2.0f;
		const float centerGroupWidth = scenePillWidth + chipSpacing + modePillWidth + chipSpacing + playButtonWidth;
		const float centerStartX = ImGui::GetWindowWidth() * 0.5f - centerGroupWidth * 0.5f;
		if (centerStartX > ImGui::GetCursorPosX() + ImGuiWrapper::S(12.0f))
			ImGui::SameLine(centerStartX);
		else
			ImGui::SameLine(0.0f, ImGuiWrapper::S(12.0f));

		EditorTheme::DrawPill(
			sceneLabel.c_str(),
			mSceneDirty ? ImVec4(0.34f, 0.23f, 0.12f, 0.96f) : ImVec4(0.13f, 0.19f, 0.28f, 0.96f));
		ImGui::SameLine(0.0f, chipSpacing);
		EditorTheme::DrawPill(
			modeLabel,
			ModeManager::IsEditState() ? ImVec4(0.14f, 0.22f, 0.31f, 0.96f) : ImVec4(0.16f, 0.31f, 0.20f, 0.96f));
		ImGui::SameLine(0.0f, chipSpacing);
		const ImGuiTextureHandle playStopIcon = ModeManager::IsEditState() ? iconManager.GetPlayIcon() : iconManager.GetStopIcon();
		ImGui::PushStyleColor(ImGuiCol_Button,
			ModeManager::IsEditState() ? ImVec4(0.16f, 0.35f, 0.20f, 0.96f) : ImVec4(0.45f, 0.16f, 0.16f, 0.96f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
			ModeManager::IsEditState() ? ImVec4(0.20f, 0.42f, 0.24f, 1.0f) : ImVec4(0.52f, 0.20f, 0.20f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive,
			ModeManager::IsEditState() ? ImVec4(0.24f, 0.48f, 0.28f, 1.0f) : ImVec4(0.58f, 0.24f, 0.24f, 1.0f));
		if (ImGui::ImageButton("##PlayStop", IconManager::ToImTextureID(playStopIcon), toolbarIconSize, ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), ImVec4(1, 1, 1, 1)))
		{
			TryChangeSceneMode(ModeManager::IsEditState());
		}
		ImGui::PopStyleColor(3);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", ModeManager::IsEditState() ? EditorLayer::GetText("Play") : EditorLayer::GetText("Stop"));

		ImGui::EndChild();
		ImGui::End();

		ImGui::PopStyleColor(5);
		ImGui::PopStyleVar(6);
	}

	void EditorLayer::UI_StatusBar()
	{
		const float statusBarHeight = ImGuiWrapper::S(30.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGuiWrapper::S(8.0f, 3.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImGuiWrapper::S(8.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.08f, 0.09f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.19f, 0.22f, 0.27f, 0.95f));

		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowViewport(viewport->ID);
		ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight));
		ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, statusBarHeight));
		if (ImGui::Begin("##StatusBar", nullptr,
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_NoDocking |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoScrollWithMouse |
			ImGuiWindowFlags_NoSavedSettings))
		{
			const char* untitledLabel = EditorLayer::GetText("Untitled");
			const std::string sceneName = GetSceneDisplayName(mEditorScenePath, untitledLabel);
			const std::string viewportSizeText = BuildViewportSizeText(ConfigManager::mViewportSize);
			const char* modeText = ModeManager::IsEditState() ? EditorLayer::GetText("Edit Mode") : EditorLayer::GetText("Play Mode");
			Entity selectedEntity = GetPanel<SceneHierarchyPanel>()->GetSelectedEntity();
			EditorTheme::DrawPill(
				sceneName.c_str(),
				mSceneDirty ? ImVec4(0.34f, 0.23f, 0.12f, 0.96f) : ImVec4(0.13f, 0.19f, 0.28f, 0.96f));
			ImGui::SameLine(0.0f, ImGuiWrapper::S(10.0f));
			if (selectedEntity && selectedEntity.HasComponent<TagComponent>())
				ImGui::Text("%s: %s", EditorLayer::GetText("Selected"), selectedEntity.GetComponent<TagComponent>().Tag.c_str());
			else
				ImGui::TextDisabled("%s", EditorLayer::GetText("No Selection"));

			if (mActiveUndoClient)
			{
				const char* dirtyText = mActiveUndoClient->IsUndoContextDirty() ? EditorLayer::GetText("Modified") : EditorLayer::GetText("Saved");
				const std::string contextText = std::string(mActiveUndoClient->GetUndoContextName()) + " | " + dirtyText;
				const float contextTextWidth = ImGui::CalcTextSize(contextText.c_str()).x;
				const float centerStartX = ImGui::GetWindowWidth() * 0.5f - contextTextWidth * 0.5f;
				if (centerStartX > ImGui::GetCursorPosX() + ImGuiWrapper::S(16.0f))
					ImGui::SameLine(centerStartX);
				else
					ImGui::SameLine(0.0f, ImGuiWrapper::S(16.0f));
				ImGui::TextDisabled("%s", contextText.c_str());
			}

			const float fpsValue = ImGui::GetIO().Framerate;
			const std::string fpsText = std::string(EditorLayer::GetText("Editor FPS")) + " " + std::to_string((int)fpsValue);
			const float modePillWidth = ImGui::CalcTextSize(modeText).x + ImGuiWrapper::S(20.0f);
			const float viewportPillWidth = ImGui::CalcTextSize(viewportSizeText.c_str()).x + ImGuiWrapper::S(20.0f);
			const float fpsTextWidth = ImGui::CalcTextSize(fpsText.c_str()).x;
			const float rightGroupWidth = modePillWidth + ImGuiWrapper::S(8.0f) + viewportPillWidth + ImGuiWrapper::S(8.0f) + fpsTextWidth;
			const float rightStartX = ImGui::GetWindowWidth() - rightGroupWidth - ImGuiWrapper::S(10.0f);
			if (rightStartX > ImGui::GetCursorPosX() + ImGuiWrapper::S(16.0f))
				ImGui::SameLine(rightStartX);
			else
				ImGui::SameLine(0.0f, ImGuiWrapper::S(16.0f));
			EditorTheme::DrawPill(
				modeText,
				ModeManager::IsEditState() ? ImVec4(0.14f, 0.22f, 0.31f, 0.96f) : ImVec4(0.16f, 0.31f, 0.20f, 0.96f));
			ImGui::SameLine(0.0f, ImGuiWrapper::S(8.0f));
			EditorTheme::DrawPill(viewportSizeText.c_str(), ImVec4(0.16f, 0.17f, 0.20f, 0.96f));
			ImGui::SameLine(0.0f, ImGuiWrapper::S(8.0f));
			ImGui::TextUnformatted(fpsText.c_str());

			ImGui::End();
		}

		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar(2);
	}

	void EditorLayer::LoadDefaultEditorConfig()
	{
		const std::filesystem::path DefaultEditorConfigPath{ ConfigManager::GetFullPath("Config/EditorLayoutDefault.ini") };
		HE_CORE_ASSERT(std::filesystem::exists(DefaultEditorConfigPath));
		ImGui::LoadIniSettingsFromDisk(DefaultEditorConfigPath.string().c_str());
		mResetEditorDockLayout = true;

		const Json defaultLayout =
			mBaseEditorJson.contains("DefaultLayout") && mBaseEditorJson["DefaultLayout"].is_object()
			? mBaseEditorJson["DefaultLayout"]
			: Json::object();

		// Window
		bShowViewport = defaultLayout.value("ShowViewport", true);
		bShowContentBrowser = defaultLayout.value("ShowContentBrowser", true);
		bShowSceneHierachy = defaultLayout.value("ShowSceneHierarchy", true);
		bShowProperties = defaultLayout.value("ShowProperties", true);
		bShowStats = defaultLayout.value("ShowStats", false);
		bShowOutputLog = defaultLayout.value("ShowOutputLog", true);
		bShowBaseSettings = defaultLayout.value("ShowBaseSettings", false);
		bShowProjectSettings = defaultLayout.value(
			"ShowProjectSettings",
			false);
		bShowRenderGraphDiagnostics = defaultLayout.value(
			"ShowRenderGraphDiagnostics",
			false);
		bShowOfflineRenderer = defaultLayout.value(
			"ShowOfflineRenderer",
			false);
		bShowSRT = defaultLayout.value("ShowSRT", true);

		// Help
		bShowTutorial = defaultLayout.value("ShowTutorial", false);
		bShowAboutMe = defaultLayout.value("ShowAboutMe", false);
		bShowDemoImGui = defaultLayout.value("ShowDemoImGui", false);
	}

	void EditorLayer::SaveDefaultEditorConfig()
	{
		ImGuiContext& g = *GImGui;
		const std::filesystem::path defaultEditorConfigPath{ ConfigManager::GetProjectFullPath("Config/EditorLayoutDefault.ini") };
		ImGui::SaveIniSettingsToDisk(defaultEditorConfigPath.string().c_str());

		Json& defaultLayout = mBaseEditorJson["DefaultLayout"];
		defaultLayout["ShowViewport"] = bShowViewport;
		defaultLayout["ShowContentBrowser"] = bShowContentBrowser;
		defaultLayout["ShowSceneHierarchy"] = bShowSceneHierachy;
		defaultLayout["ShowProperties"] = bShowProperties;
		defaultLayout["ShowStats"] = bShowStats;
		defaultLayout["ShowOutputLog"] = bShowOutputLog;
		defaultLayout["ShowBaseSettings"] = bShowBaseSettings;
		defaultLayout["ShowProjectSettings"] = bShowProjectSettings;
		defaultLayout["ShowRenderGraphDiagnostics"] =
			bShowRenderGraphDiagnostics;
		defaultLayout["ShowOfflineRenderer"] = bShowOfflineRenderer;
		defaultLayout["ShowSRT"] = bShowSRT;
		defaultLayout["ShowTutorial"] = bShowTutorial;
		defaultLayout["ShowAboutMe"] = bShowAboutMe;
		defaultLayout["ShowDemoImGui"] = bShowDemoImGui;
		SaveBaseEditorConfig();

		if (g.IO.IniFilename != NULL)
			ImGui::SaveIniSettingsToDisk(g.IO.IniFilename);

		ImGui::InsertNotification({ ImGuiToastType_Success, 3000, "%s", GetText("Saved default layout.") });
	}

	const char* EditorLayer::GetText(const char* label)
	{
		if (!mTextIni)
			return label;

		const char* value = mTextIni->GetValue(label);
		return value ? value : label;
	}

	void EditorLayer::SaveBaseEditorConfig()
	{
		auto baseEditorConfigPath = ConfigManager::GetProjectFullPath("Config/BaseEditor.json");
		Utils::SaveJsonToFile(mBaseEditorJson, baseEditorConfigPath);
	}

	void EditorLayer::OnEvent(Event& e)
    {
		mIteratingPanelMap = true;
		for (const auto& [panelTypeName, panel] : mPanelMap)
		{
			panel->OnEvent(e);
		}
		mIteratingPanelMap = false;
		FlushPendingMaterialPanels();
		FlushPendingAssetPreviewPanels();

		if (!ModeManager::IsEditState() && (bHandleEvent || mRuntimeCursorCaptured))
		{
			if (RmlUiManager::GetInstance().OnEvent(e))
				return;
		}

		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<KeyPressedEvent>(HE_BIND_EVENT_FN(EditorLayer::OnKeyPressed));
		dispatcher.Dispatch<MouseScrolledEvent>(HE_BIND_EVENT_FN(EditorLayer::OnMouseScrolled));

		if (!e.Handled && bHandleEvent)
		{
			dispatcher.Dispatch<MouseButtonPressedEvent>(HE_BIND_EVENT_FN(EditorLayer::OnMouseButtonPressed));
		}
    }

	bool EditorLayer::OnMouseScrolled(MouseScrolledEvent& e)
	{
		if (e.Handled)
			return true;

		if (!ModeManager::IsEditState() || !bViewportHovered)
			return false;

		mEditorCamera.OnEvent(e);
		return false;
	}

	bool EditorLayer::OnKeyPressed(KeyPressedEvent& e)
    {
        // Shortcuts
        if (e.GetRepeatCount() > 0)
            return false;

        bool control = Input::IsKeyPressed(Key::LeftControl) || Input::IsKeyPressed(Key::RightControl);
        bool shift = Input::IsKeyPressed(Key::LeftShift) || Input::IsKeyPressed(Key::RightShift);
		bool mouseRight = Input::IsMouseButtonPressed(Mouse::ButtonRight);

		if (!ModeManager::IsEditState())
		{
			if (e.GetKeyCode() == Key::Escape)
			{
				if (control || !RmlUiManager::GetInstance().HasActiveDocument())
				{
					TryChangeSceneMode(false);
					return true;
				}
				return false;
			}

			return false;
		}

		if (control)
		{
			switch (e.GetKeyCode())
			{
				case Key::Z:
				{
					if (ImGui::GetIO().WantTextInput)
						return false;
					if (shift)
						return RedoActiveDocument();
					return UndoActiveDocument();
				}
				case Key::Y:
				{
					if (ImGui::GetIO().WantTextInput)
						return false;
					return RedoActiveDocument();
				}
				default:
					break;
			}
		}

        switch (e.GetKeyCode())
        {
            case Key::N:
            {
                if (control)
                    NewScene();
                break;
            }
            case Key::O:
            {
                if (control)
                    OpenScene();
                break;
            }
            case Key::S:
            {
				if (control)
				{
					if (shift)
						SaveSceneAs();
					else
						SaveScene();
				}
                break;
            }

			// Level Commands
			case Key::D:
			{
				if (control)
					OnDuplicateEntity();
				break;
			}

			case Key::P:
			{
				if (control)
					PackageProjectWindows();
				break;
			}

			// Gizmos
			case Key::Q:
				if (!mouseRight)
					mGizmoType = -1;
				break;
			case Key::W:
				if (!mouseRight)
					mGizmoType = ImGuizmo::OPERATION::TRANSLATE;
				break;
			case Key::E:
				if (!mouseRight)
					mGizmoType = ImGuizmo::OPERATION::ROTATE;
				break;
			case Key::R:
				if (!mouseRight)
					mGizmoType = ImGuizmo::OPERATION::SCALE;
				break;
			case Key::End:
			{
				if (ImGui::GetIO().WantTextInput || mGizmoType != ImGuizmo::OPERATION::TRANSLATE)
					break;

				SceneHierarchyPanel* sceneHierarchyPanel = GetPanel<SceneHierarchyPanel>();
				Entity selectedEntity = sceneHierarchyPanel->GetSelectedEntity();
				if (!selectedEntity || sceneHierarchyPanel->IsSelectedEntityLocked() || !sceneHierarchyPanel->IsEntityVisible(selectedEntity))
					break;

				if (auto* physicsSystem = selectedEntity.GetLevel()->TryGetSystem<PhysicSystem3D>())
				{
					if (physicsSystem->SnapEntityToFloor(selectedEntity))
						MarkSceneDirty("Snap Entity To Floor");
				}
				break;
			}

			// Renderdoc
			case Key::F7:
			{
				Ref<IRenderCaptureService> captureService;
				ModuleManager& moduleManager = ModuleManager::GetInstance();
				if (moduleManager.IsPluginHostConfigured())
				{
					captureService = moduleManager.GetPluginManager()
						.GetServices()
						.TryGet<IRenderCaptureService>(c_RenderCaptureServiceId);
				}
				if (captureService && captureService->IsAvailable())
					captureService->Capture();
				else if (captureService)
					HE_CORE_WARN(
						"RenderDoc capture unavailable: {}",
						captureService->GetUnavailableReason());
				else
					HE_CORE_WARN("RenderDoc capture ignored because the plugin is disabled");
				break;
			}
        }

		return true;
    }

	bool EditorLayer::OnMouseButtonPressed(MouseButtonPressedEvent& e)
	{
			if (e.GetMouseButton() == Mouse::ButtonLeft)
			{
				if (bViewportHovered && !ImGuizmo::IsOver() && !bViewGizmoHovered && !bViewGizmoActive && !Input::IsKeyPressed(Key::LeftAlt))
				{
				glm::vec2 screenPos = glm::vec2{ ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y } - mViewportBounds[0];

				const float depthRange = 1.0f;  // max depth 1.0, min depth 0.0
				float width = mViewportBounds[1].x - mViewportBounds[0].x;
				float height = mViewportBounds[1].y - mViewportBounds[0].y;

				// offer to https://antongerdelan.net/opengl/raycasting.html
				glm::vec4 rayClip = { 2 * screenPos.x / width - 1, -2 * screenPos.y / height + 1, -1.0f, 1.0f };
				glm::vec4 rayEye = glm::inverse(mEditorCamera.GetProjection()) * rayClip;
				rayEye = { rayEye.x, rayEye.y, -1.0, 0.0 };
				glm::vec3 rayWorld = glm::inverse(mEditorCamera.GetViewMatrix()) * rayEye;
				rayWorld = glm::normalize(rayWorld);

				EmbreeSystem* embreeSystem = mActiveScene
					? mActiveScene->TryGetSystem<EmbreeSystem>()
					: nullptr;
				const auto hit = embreeSystem
					? embreeSystem->Raycast(mEditorCamera.GetPosition(), rayWorld)
					: std::nullopt;
				if (hit)
				{
					Entity hitEntity = mActiveScene->TryGetEntityByUUID(hit->EntityID);
					if (hitEntity && !GetPanel<SceneHierarchyPanel>()->CanSelectEntity(hitEntity))
						return false;
					if (hitEntity)
					{
						SceneHierarchyPanel::MeshSelectionContext meshSelectionContext;
						meshSelectionContext.bValid = true;
						meshSelectionContext.SubMeshIndex = hit->SubMeshIndex;
						meshSelectionContext.MaterialIndex = hit->MaterialIndex;
						meshSelectionContext.bExpandMaterialTree = true;

						GetPanel<SceneHierarchyPanel>()->SetSelectedEntity(hitEntity, meshSelectionContext);
						return false;
					}
				}
			}
		}
		return false;
	}

	void EditorLayer::ApplyViewportMode(std::string_view modeId)
	{
		Application& application = Application::GetInstance();
		const RenderCapabilities capabilities =
			application.GetRenderCapabilities();
		const ViewportMode previous =
			mViewportModeController.GetCurrentMode();
		try
		{
			const ViewportModeTransition transition =
				mViewportModeController.SetMode(
					modeId,
					capabilities,
					ModeManager::IsEditState());
			if (!transition.bChanged)
				return;

			if (!transition.Previous.UsesRasterRenderer())
				application.DeactivateOfflineViewportRenderer();
			if (!transition.Current.UsesRasterRenderer())
			{
				application.ActivateOfflineViewportRenderer(
					transition.Current.RendererProviderId);
				bShowOfflineRenderer = true;
			}

			const int desiredDimension =
				transition.Current.Dimension == SceneDimension::ThreeD ? 1 : 0;
			if (ModeManager::b3DMode != desiredDimension)
			{
				ModeManager::b3DMode = desiredDimension;
				mChangeDimensionPending = true;
			}
			mViewportModeError.clear();
			++mEditorRenderRevision;
		}
		catch (const std::exception& exception)
		{
			try
			{
				application.DeactivateOfflineViewportRenderer();
				if (!previous.UsesRasterRenderer())
				{
					application.ActivateOfflineViewportRenderer(
						previous.RendererProviderId);
				}
				(void)mViewportModeController.SetMode(
					previous.Id,
					capabilities,
					true);
			}
			catch (...)
			{
				(void)mViewportModeController.SetMode(
					"raster.3d",
					capabilities,
					true);
				ModeManager::b3DMode = 1;
				mChangeDimensionPending = true;
			}
			mViewportModeError = exception.what();
			HE_CORE_ERROR(
				"Failed to switch viewport renderer mode: {}",
				mViewportModeError);
		}
	}

	bool EditorLayer::PrepareViewportModeForPlay()
	{
		try
		{
			if (mViewportModeController.IsOfflineRendererActive())
				Application::GetInstance().DeactivateOfflineViewportRenderer();
			(void)mViewportModeController.PrepareForPlay();
			if (ModeManager::b3DMode == 0)
			{
				ModeManager::b3DMode = 1;
				if (mEditorScene)
					mEditorScene->ChangeDimMode();
			}
			mChangeDimensionPending = false;
			mViewportModeError.clear();
			++mEditorRenderRevision;
			return true;
		}
		catch (const std::exception& exception)
		{
			mViewportModeError =
				"Cannot enter Play until the offline renderer stops safely: " +
				std::string(exception.what());
			HE_CORE_ERROR("{}", mViewportModeError);
			return false;
		}
		catch (...)
		{
			mViewportModeError =
				"Cannot enter Play: offline renderer shutdown failed.";
			HE_CORE_ERROR("{}", mViewportModeError);
			return false;
		}
	}

	void EditorLayer::OnViewportResize()
	{
		if (ConfigManager::mViewportSize.x > 0.0f && ConfigManager::mViewportSize.y > 0.0f)
		{
			mEditorCamera.SetViewportSize(ConfigManager::mViewportSize.x, ConfigManager::mViewportSize.y);
			if (mActiveScene)
				mActiveScene->OnViewportResize(
					static_cast<uint32_t>(ConfigManager::mViewportSize.x),
					static_cast<uint32_t>(ConfigManager::mViewportSize.y));
			++mEditorRenderRevision;
		}
	}

	void EditorLayer::UpdateSettings()
	{
		// from imgui.cpp
		// Load settings on first frame (if not explicitly loaded manually before)
		ImGuiContext& g = *GImGui;
		if (!g.SettingsLoaded)
		{
			IM_ASSERT(g.SettingsWindows.empty());
			if (g.IO.IniFilename)
				ImGui::LoadIniSettingsFromDisk(g.IO.IniFilename);
			g.SettingsLoaded = true;
		}

		// Save settings (with a delay after the last modification, so we don't spam disk too much)
		if (g.SettingsDirtyTimer > 0.0f)
		{
			g.SettingsDirtyTimer -= g.IO.DeltaTime;
			if (g.SettingsDirtyTimer <= 0.0f)
			{
				if (g.IO.IniFilename != NULL)
					ImGui::SaveIniSettingsToDisk(g.IO.IniFilename);
				else
					g.IO.WantSaveIniSettings = true;  // Let user know they can call SaveIniSettingsToMemory(). user will need to clear io.WantSaveIniSettings themselves.
				g.SettingsDirtyTimer = 0.0f;
			}
		}
	}

    void EditorLayer::NewScene()
    {
		if (!ModeManager::IsEditState())
		{
			HE_CORE_WARN("Cannot create a scene while the editor is in Play mode.");
			return;
		}

		Ref<Level> newScene = CreateRef<Level>();
		newScene->OnViewportResize((uint32_t)ConfigManager::mViewportSize.x, (uint32_t)ConfigManager::mViewportSize.y);
		EmbreeSystem::ResetSharedContext();
		GetPanel<SceneHierarchyPanel>()->SetContext(newScene);

		mActiveScene = newScene;
		mEditorScene = mActiveScene;

		mEditorScenePath = std::filesystem::path();
		ResetSceneHistory("New Scene");

		ImGui::InsertNotification({ ImGuiToastType_Success, 3000, "成功新建场景!" });
    }

    void EditorLayer::OpenScene()
    {
		if (!ModeManager::IsEditState())
		{
			HE_CORE_WARN("Cannot open a scene while the editor is in Play mode.");
			return;
		}

		std::filesystem::path filepath = FileDialogs::OpenFile("HEngine Level (*.he)\0*.he\0\0", ConfigManager::GetFullPath("Assets/Scenes").string());
		if (!filepath.empty())
			OpenScene(filepath);
    }

	void EditorLayer::OpenScene(const std::filesystem::path& path)
	{
		if (!ModeManager::IsEditState())
		{
			HE_CORE_WARN("Cannot open scene '{}' while the editor is in Play mode.", path.string());
			return;
		}

		if (path.extension().string() != ".he")
		{
			HE_WARN("Could not load {0} - not a scene file", path.filename().string());
			return;
		}

		Ref<Level> newScene = CreateRef<Level>();
		SceneSerializer serializer(newScene);
		if (serializer.Deserialize(path.string()))
		{
			newScene->OnViewportResize((uint32_t)ConfigManager::mViewportSize.x, (uint32_t)ConfigManager::mViewportSize.y);
			EmbreeSystem::ResetSharedContext();
			GetPanel<SceneHierarchyPanel>()->SetContext(newScene);

			mEditorScene = newScene;
			mActiveScene = newScene;
			mEditorScenePath = path;
			InitializeEditorCameraFromPrimary(mEditorCamera, newScene);
			ResetSceneHistory("Open Scene");

			ImGui::InsertNotification({ ImGuiToastType_Success, 3000, "成功打开场景!" });
		}
	}

	void EditorLayer::SaveScene()
	{
		if (!mEditorScenePath.empty())
		{
			const std::filesystem::path contentRoot =
				ConfigManager::GetContentRootForPath(mEditorScenePath);
			if (contentRoot == ConfigManager::GetInstance().GetEngineContentFolder())
				SaveSceneAs();
			else
				SerializeScene(mActiveScene, mEditorScenePath);
		}
		else
			SaveSceneAs();
	}

    void EditorLayer::SaveSceneAs()
    {
        std::filesystem::path filepath = FileDialogs::SaveFile("HEngine Level (*.he)\0*.he\0\0", ConfigManager::GetFullPath("Assets/Scenes").string());
        if (!filepath.empty())
        {
			SerializeScene(mActiveScene, filepath);
			mEditorScenePath = filepath;
		}
    }

	void EditorLayer::SerializeScene(Ref<Level> scene, const std::filesystem::path& path)
	{
		SceneSerializer serializer(scene);
		serializer.Serialize(path.string());
		if (mSceneThumbnailService)
		{
			mSceneThumbnailService->Invalidate(Utils::NormalizeAssetPath(
				ConfigManager::GetRelativePath(path)));
		}
		mSavedSceneContentSnapshot = DumpJson(serializer.SerializeToJson());
		mSavedSceneHistoryCursor = mSceneHistoryCursor;
		mSceneDirty = false;

		ImGui::InsertNotification({ ImGuiToastType_Success, 3000, "成功保存场景!" });
	}

	void EditorLayer::PackageProjectWindows()
	{
		if (mPackageFuture.valid() &&
			mPackageFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			ImGui::InsertNotification({ ImGuiToastType_Warning, 3000, "打包任务正在进行中，请稍后..." });
			return;
		}

		const std::string executableName = HE_PROJECT_EXECUTABLE_NAME;
		const auto projectRoot = ConfigManager::GetInstance().GetRootFolder();
		const auto repoRoot = ConfigManager::GetEngineFolder().parent_path();
		const auto buildDir = ConfigManager::GetSavedPath("Build/Package");
		const auto packageOutputFile = buildDir / "package-output.txt";

		HE_CORE_INFO("[Package] 开始打包，发布根目录: {}", (repoRoot / "dist").string());
		ImGui::InsertNotification({ ImGuiToastType_Info, 60000, "正在编译 Runtime，请稍候..." });

		mPackageFuture = std::async(std::launch::async,
			[packageOutputFile, projectRoot, repoRoot, buildDir, executableName]() -> std::pair<bool, std::string>
		{
			std::error_code ec;
			std::filesystem::create_directories(buildDir, ec);
			if (ec)
			{
				HE_CORE_ERROR("[Package] 无法创建构建目录: {} ({})", buildDir.string(), ec.message());
				return { false, "无法创建构建目录: " + buildDir.string() };
			}
			std::filesystem::remove(packageOutputFile, ec);
			if (ec)
				return { false, "无法清理旧的打包结果: " + packageOutputFile.string() };

			const auto packageScript = repoRoot / "Tools/Agent/Package.ps1";
			if (!std::filesystem::exists(packageScript))
			{
				HE_CORE_ERROR("[Package] 找不到统一打包入口: {}", packageScript.string());
				return { false, "找不到统一打包入口: " + packageScript.string() };
			}
			const auto logFile = buildDir / "build_game.log";
			const auto batFile = buildDir / "build_game.bat";
			{
				std::ofstream bat(batFile);
				if (!bat)
					return { false, "无法创建打包启动脚本: " + batFile.string() };
				bat << "@echo off\r\n";
				bat << "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \""
					<< packageScript.string() << "\" -Project \"" << projectRoot.string()
					<< "\" -ResultFile \"" << packageOutputFile.string() << "\" > \""
					<< logFile.string() << "\" 2>&1\r\n";
				bat << "exit /b %errorlevel%\r\n";
			}

			const std::string runCmd = "\"" + batFile.string() + "\"";
			if (std::system(runCmd.c_str()) != 0)
			{
				HE_CORE_ERROR("[Package] 打包失败，详情见: {}", logFile.string());
				return { false, "打包失败，详情见: " + logFile.string() };
			}

			std::ifstream result(packageOutputFile, std::ios::binary);
			std::string packageDirText;
			if (!result || !std::getline(result, packageDirText) || packageDirText.empty())
			{
				HE_CORE_ERROR("[Package] 打包结果未报告发布目录: {}", packageOutputFile.string());
				return { false, "打包结果未报告发布目录: " + packageOutputFile.string() };
			}
			const auto packageDir = std::filesystem::u8path(packageDirText);

			const auto packagedExe = packageDir / (executableName + ".exe");
			const auto packagedPak = packageDir / "Game.pak";
			const auto packagedManifest = packageDir / "Game.manifest.json";
			if (!std::filesystem::exists(packagedExe) ||
				!std::filesystem::exists(packagedPak) ||
				!std::filesystem::exists(packagedManifest))
			{
				HE_CORE_ERROR("[Package] Cooked package is incomplete: {}", packageDir.string());
				return { false, "Cooked package is incomplete: " + packageDir.string() };
			}

			HE_CORE_INFO("[Package] 打包完成: {}", packageDir.string());
			return { true, packageDir.string() };
		});
	}

	void EditorLayer::OnScenePlay()
	{
		if (!ModeManager::IsEditState())
			return;
		if (!mEditorScene)
			throw std::logic_error("Cannot enter Play mode without an editor scene.");
		if (!PrepareViewportModeForPlay())
			return;

		Ref<Level> runtimeScene = Level::Copy(mEditorScene, true);
		try
		{
			runtimeScene->OnRuntimeStart();
			EmbreeSystem::ResetSharedContext();
			if (auto* physicsSystem = mEditorScene->TryGetSystem<PhysicSystem3D>())
				physicsSystem->ReleaseEditorDebugResources();

			mActiveScene = runtimeScene;
			GetPanel<SceneHierarchyPanel>()->SetContext(mActiveScene);
			ModeManager::ChangeState();

			mRuntimeAltDown = false;
			CenterCursorToViewport();
			SetRuntimeCursorCaptured(true);
			Application::GetWindow().SetInputMethodEnabled(false);
			(void)Application::GetInstance().GetProjectResourceWarmup().QueueGroup("Boot");
		}
		catch (...)
		{
			const std::exception_ptr originalFailure = std::current_exception();
			if (!ModeManager::IsEditState())
				ModeManager::ChangeState();
			mActiveScene = mEditorScene;

			try
			{
				GetPanel<SceneHierarchyPanel>()->SetContext(mActiveScene);
			}
			catch (...)
			{
				ReportEditorCleanupFailure("Editor scene context restoration failed after Play startup.", std::current_exception());
			}

			try
			{
				mRuntimeAltDown = false;
				SetRuntimeCursorCaptured(false);
				Application::GetWindow().SetInputMethodEnabled(true);
			}
			catch (...)
			{
				ReportEditorCleanupFailure("Editor input restoration failed after Play startup.", std::current_exception());
			}

			try
			{
				runtimeScene->OnRuntimeStop();
			}
			catch (...)
			{
				ReportEditorCleanupFailure("Editor runtime rollback failed after Play startup.", std::current_exception());
			}
			ReleaseEditorRuntimeWarmupResidency(
				"Editor resource warm-up rollback failed after Play startup.");
			std::rethrow_exception(originalFailure);
		}
	}

	void EditorLayer::OnSceneStop()
	{
		if (ModeManager::IsEditState())
			return;

		std::exception_ptr firstFailure;
		auto recordFailure = [&firstFailure](std::string_view operation) noexcept
		{
			const std::exception_ptr failure = std::current_exception();
			if (!firstFailure)
				firstFailure = failure;
			else
				ReportEditorCleanupFailure(operation, failure);
		};

		Ref<Level> runtimeScene = mActiveScene;
		try
		{
			if (runtimeScene)
				runtimeScene->OnRuntimeStop();
		}
		catch (...)
		{
			recordFailure("Editor runtime stop encountered an additional failure.");
		}

		try
		{
			EmbreeSystem::ResetSharedContext();
		}
		catch (...)
		{
			recordFailure("Editor pick-scene reset encountered an additional failure.");
		}

		mActiveScene = mEditorScene;
		try
		{
			GetPanel<SceneHierarchyPanel>()->SetContext(mActiveScene);
		}
		catch (...)
		{
			recordFailure("Editor scene context restoration encountered an additional failure.");
		}

		mRuntimeAltDown = false;
		try
		{
			SetRuntimeCursorCaptured(false);
		}
		catch (...)
		{
			recordFailure("Editor cursor restoration encountered an additional failure.");
		}

		try
		{
			Application::GetWindow().SetInputMethodEnabled(true);
		}
		catch (...)
		{
			recordFailure("Editor input method restoration encountered an additional failure.");
		}

		if (!ModeManager::IsEditState())
			ModeManager::ChangeState();
		mEditorWarmupFramesRemaining = 10;
		++mEditorRenderRevision;

		ReleaseEditorRuntimeWarmupResidency(
			"Editor resource warm-up release failed during runtime stop.");

		if (firstFailure)
			std::rethrow_exception(firstFailure);
	}

	void EditorLayer::TryChangeSceneMode(bool bPlay) noexcept
	{
		try
		{
			if (bPlay)
				OnScenePlay();
			else
				OnSceneStop();
		}
		catch (...)
		{
			ReportEditorActionFailure(
				bPlay ? "Editor Play failed" : "Editor Stop failed",
				std::current_exception());
		}
	}

	void EditorLayer::UpdateRuntimeCursorPolicy()
	{
		const bool bUiCursorVisible = !ModeManager::IsEditState() &&
			RmlUiManager::GetInstance().IsMouseCursorVisibleRequested();
		if (bUiCursorVisible)
			ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
		else
			ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;

		if (ModeManager::IsEditState())
		{
			mRuntimeAltDown = false;
			SetRuntimeCursorCaptured(false);
			return;
		}

		const bool bAltDown = Input::IsKeyPressed(Key::LeftAlt) || Input::IsKeyPressed(Key::RightAlt);
		if (bAltDown && !mRuntimeAltDown)
			CenterCursorToViewport();

		SetRuntimeCursorCaptured(!bAltDown && !bUiCursorVisible);
		mRuntimeAltDown = bAltDown;
	}

	void EditorLayer::SetRuntimeCursorCaptured(bool bCaptured)
	{
		if (mRuntimeCursorCaptured == bCaptured)
			return;

		if (bCaptured)
			CenterCursorToViewport();

		Application::GetWindow().SetMouseCursorCaptured(bCaptured);

		mRuntimeCursorCaptured = bCaptured;
	}

	void EditorLayer::CenterCursorToViewport()
	{
		float cursorX = 0.0f;
		float cursorY = 0.0f;

		const float viewportWidth = mViewportBounds[1].x - mViewportBounds[0].x;
		const float viewportHeight = mViewportBounds[1].y - mViewportBounds[0].y;
		if (viewportWidth > 1.0f && viewportHeight > 1.0f)
		{
			int windowPosX = 0;
			int windowPosY = 0;
			Application::GetWindow().GetWindowScreenPosition(windowPosX, windowPosY);

			const float centerScreenX = mViewportBounds[0].x + viewportWidth * 0.5f;
			const float centerScreenY = mViewportBounds[0].y + viewportHeight * 0.5f;
			cursorX = centerScreenX - static_cast<float>(windowPosX);
			cursorY = centerScreenY - static_cast<float>(windowPosY);
		}
		else
		{
			cursorX = static_cast<float>(Application::GetWindow().GetWidth()) * 0.5f;
			cursorY = static_cast<float>(Application::GetWindow().GetHeight()) * 0.5f;
		}

		Application::GetWindow().SetMouseCursorPosition(cursorX, cursorY);
	}

	void EditorLayer::OnDuplicateEntity()
	{
		if (!ModeManager::IsEditState())
			return;

		SceneHierarchyPanel* hierarchyPanel = GetPanel<SceneHierarchyPanel>();
		Entity selectedEntity = hierarchyPanel->GetSelectedEntity();
		if (selectedEntity)
		{
			Entity duplicatedEntity = hierarchyPanel->DuplicateEntityHierarchy(selectedEntity);
			if (duplicatedEntity)
				hierarchyPanel->SetSelectedEntity(duplicatedEntity);
			MarkSceneDirty("Duplicate Entity");
		}
	}

	void EditorLayer::CaptureSceneHistory(const char* label, bool bInteractive)
	{
		if (!ModeManager::IsEditState() || !mEditorScene)
			return;

		if (mSceneHistoryEntries.empty())
			ResetSceneHistory("Scene Loaded");

		if (bInteractive)
		{
			mSceneHistoryInteractivePending = true;
			mPendingSceneHistoryLabel = label ? label : "Edit Scene";
			return;
		}

		if (mSceneHistoryInteractivePending)
		{
			mSceneHistoryInteractivePending = false;
			mPendingSceneHistoryLabel.clear();
		}

		SceneHierarchyPanel* sceneHierarchyPanel = GetPanel<SceneHierarchyPanel>();
		Json editorState = sceneHierarchyPanel->ExportEditorState();
		Entity selectedEntity = sceneHierarchyPanel->GetSelectedEntity();

		SceneHistoryEntry entry;
		entry.Scene = Level::Copy(mEditorScene);
		entry.EditorState = std::move(editorState);
		entry.SelectedEntityUuid = selectedEntity ? selectedEntity.UUID().str() : std::string();
		entry.MeshSelectionContext = sceneHierarchyPanel->GetMeshSelectionContext();
		entry.Label = label ? label : "Edit Scene";

		if ((mSceneHistoryCursor + 1) < mSceneHistoryEntries.size())
		{
			if (mSavedSceneHistoryCursor > mSceneHistoryCursor)
				mSavedSceneHistoryCursor = std::numeric_limits<size_t>::max();
			mSceneHistoryEntries.erase(mSceneHistoryEntries.begin() + static_cast<ptrdiff_t>(mSceneHistoryCursor + 1), mSceneHistoryEntries.end());
		}

		mSceneHistoryEntries.push_back(std::move(entry));
		if (mSceneHistoryEntries.size() > 256)
		{
			if (mSavedSceneHistoryCursor == 0)
				mSavedSceneHistoryCursor = std::numeric_limits<size_t>::max();
			else if (mSavedSceneHistoryCursor != std::numeric_limits<size_t>::max())
				--mSavedSceneHistoryCursor;
			mSceneHistoryEntries.erase(mSceneHistoryEntries.begin());
		}
		mSceneHistoryCursor = mSceneHistoryEntries.empty() ? 0 : (mSceneHistoryEntries.size() - 1);
		mSceneDirty = mSavedSceneHistoryCursor == std::numeric_limits<size_t>::max() ||
			mSceneHistoryCursor != mSavedSceneHistoryCursor;
	}

	void EditorLayer::CommitPendingSceneHistory()
	{
		if (!mSceneHistoryInteractivePending)
			return;

		const std::string pendingLabel = mPendingSceneHistoryLabel.empty() ? std::string("Edit Scene") : mPendingSceneHistoryLabel;
		mSceneHistoryInteractivePending = false;
		mPendingSceneHistoryLabel.clear();
		CaptureSceneHistory(pendingLabel.c_str(), false);
	}

	void EditorLayer::MarkSceneDirty(const char* label, bool bInteractive)
	{
		mSceneDirty = true;
		++mEditorRenderRevision;
		CaptureSceneHistory(label, bInteractive);
	}

	void EditorLayer::NotifyUndoClientActivity(IEditorUndoClient* client, bool bFocused, bool bHovered)
	{
		if (!client)
			return;

		if (bFocused)
			mFocusedUndoClient = client;
		if (bHovered)
			mHoveredUndoClient = client;
	}

	void EditorLayer::ResolveActiveUndoClient()
	{
		IEditorUndoClient* activeClient = nullptr;
		if (IsUndoClientAvailable(mFocusedUndoClient))
			activeClient = mFocusedUndoClient;
		else if (IsUndoClientAvailable(mHoveredUndoClient))
			activeClient = mHoveredUndoClient;
		else if (IsUndoClientAvailable(mLastActiveUndoClient))
			activeClient = mLastActiveUndoClient;
		else
			activeClient = this;

		mActiveUndoClient = activeClient;
		mLastActiveUndoClient = activeClient;
	}

	bool EditorLayer::IsUndoClientAvailable(IEditorUndoClient* client) const
	{
		if (!client)
			return false;
		if (client == this)
			return true;

		for (const auto& [panelTypeName, panel] : mPanelMap)
		{
			if (panel && panel->IsOpen() && panel->GetUndoClient() == client)
				return true;
		}

		return false;
	}

	bool EditorLayer::ApplySceneSnapshot(const Ref<Level>& snapshot, const Json& editorState, const std::string& selectedUuid, const SceneHierarchyPanel::MeshSelectionContext& meshSelectionContext)
	{
		if (!snapshot)
			return false;

		SceneHierarchyPanel* sceneHierarchyPanel = GetPanel<SceneHierarchyPanel>();
		Ref<Level> restoredScene = Level::Copy(snapshot);
		restoredScene->OnViewportResize((uint32_t)ConfigManager::mViewportSize.x, (uint32_t)ConfigManager::mViewportSize.y);
		mEditorScene = restoredScene;
		mActiveScene = restoredScene;
		sceneHierarchyPanel->SetContext(mEditorScene);
		sceneHierarchyPanel->ImportEditorState(editorState);

		if (!selectedUuid.empty())
		{
			Entity restoredSelectedEntity = mEditorScene->TryGetEntityByUUID(UUID(selectedUuid));
			if (restoredSelectedEntity)
				sceneHierarchyPanel->SetSelectedEntity(restoredSelectedEntity, meshSelectionContext);
		}
		mEditorWarmupFramesRemaining = 10;
		++mEditorRenderRevision;

		return true;
	}

	void EditorLayer::ResetSceneHistory(const char* label)
	{
		if (!mEditorScene)
			return;

		SceneSerializer serializer(mEditorScene);
		mSavedSceneContentSnapshot = DumpJson(serializer.SerializeToJson());

		SceneHierarchyPanel* sceneHierarchyPanel = GetPanel<SceneHierarchyPanel>();
		SceneHistoryEntry entry;
		entry.Scene = Level::Copy(mEditorScene);
		entry.EditorState = sceneHierarchyPanel->ExportEditorState();
		Entity selectedEntity = sceneHierarchyPanel->GetSelectedEntity();
		entry.SelectedEntityUuid = selectedEntity ? selectedEntity.UUID().str() : std::string();
		entry.MeshSelectionContext = sceneHierarchyPanel->GetMeshSelectionContext();
		entry.Label = label ? label : "Scene Reset";

		mSceneHistoryEntries.clear();
		mSceneHistoryEntries.push_back(std::move(entry));
		mSceneHistoryCursor = 0;
		mSavedSceneHistoryCursor = 0;
		mSceneHistoryInteractivePending = false;
		mPendingSceneHistoryLabel.clear();
		mSceneDirty = false;
		mEditorWarmupFramesRemaining = 30;
		++mEditorRenderRevision;
	}

	bool EditorLayer::UndoActiveDocument()
	{
		return mActiveUndoClient ? mActiveUndoClient->Undo() : false;
	}

	bool EditorLayer::RedoActiveDocument()
	{
		return mActiveUndoClient ? mActiveUndoClient->Redo() : false;
	}

	bool EditorLayer::CanUndo() const
	{
		return ModeManager::IsEditState() && mSceneHistoryEntries.size() > 1 && mSceneHistoryCursor > 0;
	}

	bool EditorLayer::CanRedo() const
	{
		return ModeManager::IsEditState() && !mSceneHistoryEntries.empty() && (mSceneHistoryCursor + 1) < mSceneHistoryEntries.size();
	}

	bool EditorLayer::Undo()
	{
		if (!ModeManager::IsEditState())
			return false;
		if (mSceneHistoryInteractivePending)
			CommitPendingSceneHistory();
		if (!CanUndo())
			return false;

		const size_t targetCursor = mSceneHistoryCursor - 1;
		const SceneHistoryEntry& entry = mSceneHistoryEntries[targetCursor];
		if (!ApplySceneSnapshot(entry.Scene, entry.EditorState, entry.SelectedEntityUuid, entry.MeshSelectionContext))
			return false;

		mSceneHistoryCursor = targetCursor;
		mSceneDirty = mSavedSceneHistoryCursor == std::numeric_limits<size_t>::max() ||
			mSceneHistoryCursor != mSavedSceneHistoryCursor;
		return true;
	}

	bool EditorLayer::Redo()
	{
		if (!ModeManager::IsEditState())
			return false;
		if (mSceneHistoryInteractivePending)
			CommitPendingSceneHistory();
		if (!CanRedo())
			return false;

		const size_t targetCursor = mSceneHistoryCursor + 1;
		const SceneHistoryEntry& entry = mSceneHistoryEntries[targetCursor];
		if (!ApplySceneSnapshot(entry.Scene, entry.EditorState, entry.SelectedEntityUuid, entry.MeshSelectionContext))
			return false;

		mSceneHistoryCursor = targetCursor;
		mSceneDirty = mSavedSceneHistoryCursor == std::numeric_limits<size_t>::max() ||
			mSceneHistoryCursor != mSavedSceneHistoryCursor;
		return true;
	}

	bool EditorLayer::IsUndoContextDirty() const
	{
		return ModeManager::IsEditState() && mSceneDirty;
	}
}
