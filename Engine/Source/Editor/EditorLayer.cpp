#include "EditorLayer.h"
#include "EditorTheme.h"
#include "ImGuiWrapper/ImGuiWrapper.h"
#include "Manager/IconManager.h"
#include "Panels/BasePanel.h"
#include "Panels/BuildPublishPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/OutputLogPanel.h"
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/ScriptInspector.h"
#include "Runtime/Scripting/ScriptBackendRegistry.h"
#include "Runtime/EcsFramework/System/Script/ScriptSystem.h"
#include "Panels/MaterialPanel.h"
#include "Panels/AnimControllerPanel.h"
#include "Panels/AssetPreviewPanel.h"
#include "Panels/UIPanel.h"
#include "Panels/AudioPanel.h"
#include "Panels/BaseSettingsPanel.h"
#include "Panels/ProjectSettingsPanel.h"
#include "Panels/PluginPanel.h"
#include "Panels/RenderGraphDiagnosticsPanel.h"
#include "Panels/OfflineRendererPanel.h"
#include "Preview/MaterialThumbnailService.h"
#include "Runtime/EcsFramework/Serializer/SceneSerializer.h"
#include "Runtime/EcsFramework/Serializer/SceneDocument.h"
#include "Runtime/EcsFramework/Serializer/SceneEditingLevel.h"
#if HE_ENABLE_GM
#include "Runtime/DevTools/GM/GMService.h"
#endif
#include "Runtime/EcsFramework/Component/Basic/TagComponent.h"
#include "Runtime/EcsFramework/Component/Basic/IDComponent.h"
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
#include "Runtime/Graphics/Renderer/RenderPipeline.h"
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
#include "Runtime/Scripting/ScriptInputContext.h"
#include "Runtime/Core/Input/InputActions.h"
#include "Runtime/EcsFramework/System/UI/UIViewportContext.h"
#include "Runtime/UI/Rml/RmlUiManager.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/Core/SimulationClock.h"
#include "Runtime/Core/Modules/ModuleManager.h"
#include "Runtime/Core/Plugins/Services/RenderCaptureService.h"
#include "Runtime/Core/Math/Math.h"

#include <cstdlib>
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
#include <chrono>
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
        std::string EditorShortcutLabel(std::string_view label)
        {
            std::string result(label);
#if defined(__APPLE__)
            for (std::size_t position = 0; (position = result.find("Ctrl+", position)) != std::string::npos;)
            {
                result.replace(position, 5, "Cmd+");
                position += 4;
            }
#endif
            return result;
        }

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
	static bool bShowPlugins = false;
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
		Log::GetCoreLogger()->sinks().push_back(CreateRef<my_sink_st>());
		Log::GetCoreLogger()->set_level(spdlog::level::trace);
		if (!Log::InstallFileSink(ConfigManager::GetSavedPath("Logs/Editor.log")))
			HE_CORE_WARN("Could not install the editor diagnostic log.");
	}

    void EditorLayer::OnAttach()
    {
		// Editor panels stay inside one native window. This also avoids the
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
		RegisterPanel<PluginPanel>("Plugin Manager", &bShowPlugins);
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

		// Migrate legacy/default layouts before the first DockSpace is submitted.
		// Doing this during detach leaves a fresh installation with collapsed panes.
		mResetEditorDockLayout = ImGui::FindWindowSettingsByID(ImHashStr("##HEditorLayoutV2")) == nullptr;
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

#if WITH_EDITOR
        if (runOptions.ExitAfterFrames > 0 && std::getenv("HE_EDITOR_SETTINGS_CAPTURE"))
            bShowProjectSettings = true;
#endif

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
        if (!runOptions.StartupViewportMode.empty())
            ApplyViewportMode(runOptions.StartupViewportMode);
#if HE_ENABLE_GM
		for (const std::string action : {"describe","query","plan","apply","diff","validate","save","undo","redo","result","export_prefab"})
		{
			DevCommandDescription description;
			description.Name = "scene." + action; description.DisplayName = description.Name;
			description.Summary = action == "export_prefab"
                ? "Export current author subtree to a new Assets .hprefab. Required: sceneId, baseRevision, requestId, entity, path; never overwrite. Read scene.result with ticket."
                : "Shared author scene service; queued operations complete at OnPreUpdate. Read scene.result with ticket.";
			description.Category = "Scene"; description.Owner = "editor.scene";
			description.bUseTypedArguments = false;
			(void)GMService::GetInstance().GetCommandRegistry().Register(std::move(description),
				[this, action](DevCommandExecutionContext& context) {
					return DevCommandResult::Success(QueueSceneEditingRequest(action, context.Arguments));
				});
		}
#endif

        mSceneMailbox.Initialize(runOptions.ExitAfterFrames > 0);
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
        if (std::exchange(mInvalidateClosedAssetPanels, false))
        {
            mActiveUndoClient = nullptr;
            std::erase_if(mPanelMap, [](const auto& item) {
                const auto* panel = item.second.get();
                return !panel->IsOpen() && (dynamic_cast<const MaterialPanel*>(panel) ||
                    dynamic_cast<const AnimControllerPanel*>(panel) || dynamic_cast<const UIPanel*>(panel) ||
                    dynamic_cast<const AssetPreviewPanel*>(panel));
            });
        }
        ProcessRuntimeSceneRequest();
        // Project Manager waits for an initialized scene and a completed UI frame,
        // rather than treating process creation or the splash window as readiness.
        if (!mStartupReadyReported && mPendingSceneOpenPath.empty() &&
            Application::GetInstance().GetCompletedFrameCount() > 0)
        {
            if (mEditorScenePath.empty())
                HE_CORE_ERROR("[ProjectManager] Editor startup failed: initial scene did not load");
            else
                HE_CORE_INFO("[ProjectManager] Editor ready");
            mStartupReadyReported = true;
        }
        mSceneMailbox.SetVfxSceneContext(mActiveScene.get(), !mPendingSceneOpenPath.empty() ||
            !mPendingSceneEdits.empty() || !mPendingSceneHistoryLabel.empty() || mSceneHistoryInteractivePending);
        mSceneMailbox.Poll([this](const std::filesystem::path& path) { RequestOpenScene(path); },
            [this](const Json& params) -> Json {
                const std::string path = params.at("path");
                if (params.at("action") == "open")
                {
                    if (std::filesystem::path(path).extension() == ".hanimctrl") CreateAnimControllerPanel(path);
                    else CreateAssetPreviewPanel(path);
                    return {{"opened",true},{"path",path}};
                }
                if (std::filesystem::path(path).extension() == ".hanimctrl")
                {
                    const auto found = mPanelMap.find(AnimControllerPanel::BuildPanelKey(path));
                    if (found == mPanelMap.end()) throw std::invalid_argument("Controller panel has not been opened.");
                    auto* panel = dynamic_cast<AnimControllerPanel*>(found->second.get());
                    if (!panel) throw std::runtime_error("Requested panel is not an animation controller.");
                    return panel->AutomationGraph(params);
                }
                const auto found = mPanelMap.find(AssetPreviewPanel::BuildPanelKey(path));
                if (found == mPanelMap.end()) throw std::invalid_argument("Animation preview has not been opened.");
                auto* panel = dynamic_cast<AssetPreviewPanel*>(found->second.get());
                if (!panel) throw std::runtime_error("Requested panel is not an animation asset preview.");
                if (params.at("action") == "close")
                {
                    mPanelMap.erase(found);
                    return {{"closed",true},{"path",path}};
                }
                return panel->AutomationPreview(params);
            }, [this](const Json& params) -> Json {
                const std::string path = params.at("path");
                const std::string action = params.at("action");
                const std::string key = BuildMaterialPanelKey(path);
                if (action == "open") CreateMaterialPanel(path);
                const auto found = mPanelMap.find(key);
                if (found == mPanelMap.end()) throw std::invalid_argument("Material panel has not been opened");
                auto* panel = dynamic_cast<MaterialPanel*>(found->second.get());
                if (!panel) throw std::runtime_error("Requested panel is not a material panel");
                if (action == "close")
                {
                    if (panel->HasUnsavedChanges()) throw std::invalid_argument("Save or revert material before closing");
                    mPanelMap.erase(found);
                    return {{"closed",true},{"path",path}};
                }
                return panel->AutomationMaterial(params);
            });
		{
			HE_PROFILE_SECTION("Shader Watcher");
			shaderWatcher.Update();
		}
		if (!mPendingSceneOpenPath.empty() && !mSceneMailbox.IsVfxDiagnosticActive())
		{
			const auto path = std::exchange(mPendingSceneOpenPath, {});
			OpenScene(path);
		}
        if (mSceneMailbox.IsVfxDiagnosticActive()) return; // Defer queued author transactions until normal end.
		HE_PROFILE_SECTION("Scene Editing Observation");
		if (!mSceneEditing || !mEditorScene) return;
		mSceneEditing->SetPlaying(!ModeManager::IsEditState());
		const auto observationTime = std::chrono::steady_clock::now();
		const bool bKnownSceneEdit = !mPendingSceneHistoryLabel.empty();
		// Author edits and queued commands need a complete, current document.
		// Idle legacy-write detection checks a few entities per frame so large
		// scenes do not incur a periodic full-scene serialization stall.
		if (ModeManager::IsEditState() && !mSceneHistoryInteractivePending)
		{
			if (bKnownSceneEdit || !mPendingSceneEdits.empty())
			{
				mSceneEditing->Observe(SceneSerializer(mEditorScene).SerializeToJson(), bKnownSceneEdit);
				mPendingSceneHistoryLabel.clear();
				mSceneObservationHandles.clear();
				mSceneObservationSeen.clear();
				mSceneObservationIndex = 0;
				mNextSceneObservation = observationTime + std::chrono::milliseconds(500);
			}
			else if (!mSceneObservationHandles.empty() || observationTime >= mNextSceneObservation)
			{
				if (mSceneObservationHandles.empty())
				{
					for (const auto handle : mEditorScene->GetAllEntitiesWith<IDComponent>())
						mSceneObservationHandles.push_back(handle);
				}
				const auto& document = mSceneEditing->GetDocument();
				const SceneSerializer serializer(mEditorScene);
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(1500);
				bool bUnreportedEdit = false;
				for (size_t processed = 0; mSceneObservationIndex < mSceneObservationHandles.size() && processed < 8; ++processed)
				{
					const auto handle = mSceneObservationHandles[mSceneObservationIndex++];
					if (!mEditorScene->mRegistry.valid(handle) || !mEditorScene->mRegistry.all_of<IDComponent>(handle))
						continue;
					Entity entity(handle, mEditorScene.get());
					const std::string id = entity.UUID().str();
					mSceneObservationSeen.insert(id);
					const auto previous = document.find(id);
					if (previous == document.end()) bUnreportedEdit = true;
					else
					{
						Json current = serializer.SerializeEntityToJson(entity);
						if (current.contains("CameraComponent") && previous->contains("CameraComponent"))
						{
							auto& camera = current["CameraComponent"]["Camera"];
							const auto& previousCamera = (*previous)["CameraComponent"]["Camera"];
							if (previousCamera.contains("mAspectRatio")) camera["mAspectRatio"] = previousCamera["mAspectRatio"];
							else camera.erase("mAspectRatio");
						}
						bUnreportedEdit = current != *previous;
						if (bUnreportedEdit)
						{
							for (const auto& [component, value] : current.items())
								if (!previous->contains(component) || (*previous)[component] != value)
								{
									if (!previous->contains(component))
									{
										HE_CORE_WARN("Unreported scene change: entity={}, component={} added", id, component);
										break;
									}
									for (const auto& [field, fieldValue] : value.items())
										if (!(*previous)[component].contains(field) || (*previous)[component][field] != fieldValue)
											HE_CORE_WARN("Unreported scene change: entity={}, component={}, field={}", id, component, field);
									break;
								}
						}
					}
					if (bUnreportedEdit || std::chrono::steady_clock::now() >= deadline) break;
				}
				if (mSceneObservationIndex == mSceneObservationHandles.size() &&
					mSceneObservationSeen.size() != document.size()) bUnreportedEdit = true;
				if (bUnreportedEdit || mSceneObservationIndex == mSceneObservationHandles.size())
				{
					if (bUnreportedEdit)
						mSceneEditing->Observe(serializer.SerializeToJson(), false);
					mSceneObservationHandles.clear();
					mSceneObservationSeen.clear();
					mSceneObservationIndex = 0;
					mNextSceneObservation = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
				}
			}
		}
		if (!mSceneHistoryInteractivePending)
		{
			for (auto& pending : mPendingSceneEdits)
			{
				if (pending.LocalUi) pending.Request["baseRevision"] = mSceneEditing->GetRevision();
				Json result = mSceneEditing->Execute(pending.Method, pending.Request);
                if (pending.LocalUi)
                    mLastSceneEditError = result.value("ok", false) ? std::string{} :
                        result.value("error", Json::object()).dump().substr(0, 4096);
				if (!result.value("ok",false)) HE_CORE_WARN("Scene edit failed: {0}", result.dump());
				else if (pending.Method == "save")
				{
					mEditorScenePath = ConfigManager::GetProjectFullPath(result.at("path").get<std::string>());
					if (mSceneThumbnailService) mSceneThumbnailService->Invalidate(result.at("path").get<std::string>());
					HE_CORE_INFO("Scene saved: {0}", result.at("path").get<std::string>());
				}
				mSceneEditResults[pending.Ticket] = std::move(result);
			}
			mPendingSceneEdits.clear();
			while (mSceneEditResults.size() > 64) mSceneEditResults.erase(mSceneEditResults.begin());
		}
		const bool bSceneDirty = mSceneEditing->IsDirty();
		if (bSceneDirty != mSceneDirty)
			HE_CORE_INFO("Scene document state: {0}; dirty={1}", mEditorScenePath.generic_string(), bSceneDirty);
		mSceneDirty = bSceneDirty;
	}

	void EditorLayer::OnDetach()
	{
        StopScriptCompilation();
        if (!ModeManager::IsEditState() && mActiveScene)
            if (auto* scripts = mActiveScene->TryGetSystem<ScriptSystem>()) scripts->NotifyApplicationQuit();
        if (mBuildPublishPanel) mBuildPublishPanel->Shutdown();
        (void)mSceneMailbox.ShutdownVfx();
#if HE_ENABLE_GM
		(void)GMService::GetInstance().UnregisterOwner("editor.scene");
#endif
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
		ReleaseEditorRuntimeWarmupResidency(
			"Editor resource warm-up release failed during detach.");

		try
		{
			mRuntimeInputEngaged = false;
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
		mSceneEditing.reset();
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
        static const bool bTimeEditorUpdate = [] {
            const char* setting = std::getenv("HE_EDITOR_UPDATE_TIMINGS");
            return setting && setting[0] == '1' && setting[1] == '\0';
        }();
        const auto timingStart = bTimeEditorUpdate ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        double sceneUpdateMilliseconds = 0.0;
        double worldSubmitMilliseconds = 0.0;
        double thumbnailMilliseconds = 0.0;
        bool bEditorSceneUpdated = false;
        if (auto compiled = PollScriptCompilation())
        {
            mPendingScriptGeneration = compiled->Directory;
            if (compiled->RestartPlay && !ModeManager::IsEditState())
            {
                TryChangeSceneMode(false);
                // Play resets and loads the pending generation before creating instances.
                if (ModeManager::IsEditState()) TryChangeSceneMode(true);
            }
        }
        if (ModeManager::IsEditState() && !mPendingScriptGeneration.empty())
        {
            const auto directory = std::exchange(mPendingScriptGeneration, {});
            try
            {
                auto backend = ScriptBackendRegistry::Find("he.script.csharp");
                if (auto* reloader = dynamic_cast<IScriptReloader*>(backend.get())) reloader->Reload(directory);
            }
            catch (const std::exception& error) { HE_CORE_ERROR("Script reload failed: {0}", error.what()); }
        }
        HE_PROFILE;
        if (mSceneMailbox.IsVfxDiagnosticActive() && mViewportModeController.IsOfflineRendererActive())
        {
            (void)mSceneMailbox.ShutdownVfx();
            throw std::logic_error("Editor VFX diagnostic requires the realtime raster viewport");
        }
        mSceneMailbox.BeginVfxFrame(mActiveScene.get());

        if (mViewportModeController.IsOfflineRendererActive() &&
            !OfflineViewportRendererProviderRegistry::Get().Contains(
                mViewportModeController.GetCurrentMode().RendererProviderId))
        {
            ApplyViewportMode("raster.3d");
            mViewportModeError = "The viewport renderer provider was disabled; returned to 3D.";
        }

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
			const bool bUpdateEditorScene = mSceneMailbox.IsVfxDiagnosticActive() ? mSceneMailbox.HasVfxStep() :
				mEditorWarmupFramesRemaining > 0 ||
				bCameraChanged ||
				bEditorStateChanged ||
				ModeManager::bRealtimePreview ||
				mActiveScene->RequiresContinuousEditorUpdate();

			if (bUpdateEditorScene)
			{
				bEditorSceneUpdated = true;
				// Time-driven materials only need a fresh RenderWorld. Their shader
				// time comes from PresentationClock during extraction, so keep the
				// editor timestep paused unless the user enabled realtime preview.
				const Timestep editorTimestep = mSceneMailbox.HasVfxStep() ? Timestep(1.0f / 60.0f) :
                    ModeManager::bRealtimePreview ? ts : Timestep(0.0f);
				try
                {
                    const auto sceneStart = bTimeEditorUpdate ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{};
                    mActiveScene->OnUpdateEditor(editorTimestep, mEditorCamera);
                    const auto submitStart = bTimeEditorUpdate ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{};
                    Application::GetInstance().SubmitRenderWorld(AcquireMergedRenderWorld(*mActiveScene));
                    if (bTimeEditorUpdate)
                    {
                        const auto submitEnd = std::chrono::steady_clock::now();
                        sceneUpdateMilliseconds = std::chrono::duration<double, std::milli>(
                            submitStart - sceneStart).count();
                        worldSubmitMilliseconds = std::chrono::duration<double, std::milli>(
                            submitEnd - submitStart).count();
                    }
                    mSceneMailbox.CompleteVfxFrame(mActiveScene.get());
                }
                catch (...)
                {
                    (void)mSceneMailbox.ShutdownVfx();
                    throw;
                }
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
                if (!mScriptPlay.IsPaused() || mScriptPlay.NeedsStep())
                    mActiveScene->OnUpdateRuntime(mScriptPlay.NeedsStep() ? Timestep(1.0f / 60.0f) : ts);
                if (mScriptPlay.NeedsStep()) mScriptPlay.CompleteStep();
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

		const auto thumbnailStart = bTimeEditorUpdate ? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};
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
        if (bTimeEditorUpdate)
        {
            const auto end = std::chrono::steady_clock::now();
            const double totalMilliseconds = std::chrono::duration<double, std::milli>(
                end - timingStart).count();
            if (totalMilliseconds > 14.0)
            {
                thumbnailMilliseconds = std::chrono::duration<double, std::milli>(
                    end - thumbnailStart).count();
                HE_CORE_WARN("[EditorUpdateTiming] frame={} totalMs={} sceneUpdated={} sceneMs={} submitMs={} thumbnailMs={} otherMs={}",
                    SimulationClock::GetInstance().GetFrameNumber(), totalMilliseconds,
                    bEditorSceneUpdated, sceneUpdateMilliseconds, worldSubmitMilliseconds,
                    thumbnailMilliseconds,
                    totalMilliseconds - sceneUpdateMilliseconds - worldSubmitMilliseconds - thumbnailMilliseconds);
            }
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
			// Remove the obsolete docked toolbar without disturbing other panels.
			if (auto* toolbar = ImGui::FindWindowByName("##toolbar"); toolbar && toolbar->DockNode)
				ImGui::DockBuilderRemoveNode(toolbar->DockNode->ID);
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGuiWrapper::S(44.0f));
            ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
        }

        style.WindowMinSize.x = minWinSizeX;

		// ----MenuBar Begin----
		if (ImGui::BeginMenuBar())
		{
            if (ImGui::BeginMenu(GetText("File")))
            {
#if defined(__APPLE__)
                if (ImGui::MenuItem(ConfigManager::GetLanguage() == Language::Chinese
                    ? "在 Finder 中显示项目…" : "Show Project in Finder..."))
                    FileDialogs::OpenInExplorer(ConfigManager::GetInstance().GetRootFolder().string());
#else
                if (ImGui::MenuItem(ConfigManager::GetLanguage() == Language::Chinese
                    ? "项目管理…" : "Project Manager..."))
                {
                    const auto launcher = ConfigManager::GetEngineFolder().parent_path() / "Win-ProjectManager.bat";
                    if (!FileDialogs::OpenFileWithApplication(launcher,
                        ConfigManager::GetProjectFullPath("Project.heproject")))
                        HE_CORE_ERROR("Cannot open Project Manager: {}", launcher.string());
                }
#endif
                ImGui::Separator();
				ImGui::BeginDisabled(!ModeManager::IsEditState());
                if (ImGui::MenuItem(GetText("New"), EditorShortcutLabel("Ctrl+N").c_str()))
                    NewScene();

                ImGui::EndDisabled();

                if (ImGui::MenuItem(GetText("Open..."), EditorShortcutLabel("Ctrl+O").c_str()))
                    OpenScene();

                ImGui::BeginDisabled(!ModeManager::IsEditState());
				if (ImGui::MenuItem(GetText("Save"), EditorShortcutLabel("Ctrl+S").c_str()))
					SaveScene();

                if (ImGui::MenuItem(GetText("Save As..."), EditorShortcutLabel("Ctrl+Shift+S").c_str()))
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
				if (ImGui::MenuItem(GetText("Undo"), EditorShortcutLabel("Ctrl+Z").c_str()))
					UndoActiveDocument();
				if (!bCanUndo)
					ImGui::EndDisabled();

				if (!bCanRedo)
					ImGui::BeginDisabled();
				if (ImGui::MenuItem(GetText("Redo"), EditorShortcutLabel("Ctrl+Y / Ctrl+Shift+Z").c_str()))
					RedoActiveDocument();
				if (!bCanRedo)
					ImGui::EndDisabled();

				ImGui::Separator();
				ImGui::BeginDisabled(!ModeManager::IsEditState());
				if (ImGui::MenuItem(GetText("Duplicate Entity"), EditorShortcutLabel("Ctrl+D").c_str()))
					OnDuplicateEntity();
				ImGui::EndDisabled();

				ImGui::EndMenu();
			}
			if (ImGui::MenuItem(ConfigManager::GetLanguage() == Language::Chinese ? "发布" : "Publish", EditorShortcutLabel("Ctrl+P").c_str(), false, ModeManager::IsEditState()))
                OpenBuildPublish();
			if (ImGui::BeginMenu(GetText("Plugins")))
			{
				ImGui::MenuItem(GetText("Plugin Manager"), nullptr, &bShowPlugins);
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
					RegisterPanel<PluginPanel>("Plugin Manager", &bShowPlugins);
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
                ImGui::MenuItem(GetText("Tutorial"), nullptr, &bShowTutorial);
                ImGui::MenuItem(GetText("About Me"), nullptr, &bShowAboutMe);
                ImGuiWrapper::SeparatorWithText(ConfigManager::GetLanguage() == Language::Chinese ? "示例" : "Examples");
                if (ImGui::MenuItem(GetText("Material Showcase"), nullptr, false, ModeManager::IsEditState()))
                    RequestOpenScene(ConfigManager::GetEngineContentFullPath(c_MaterialShowcaseScenePath));
                ImGui::MenuItem(GetText("Demo ImGui"), nullptr, &bShowDemoImGui);

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
            ImGuiWrapper::ConstrainNextWindowToWorkArea(statsWindowName.c_str(), ImVec2(460, 600), ImVec2(340, 320));
            if (ImGui::Begin(statsWindowName.c_str(), &bShowStats))
            {
                const auto& stats = Application::GetInstance().GetMainSceneStats();
                const float fps = ImGui::GetIO().Framerate;
                ImGui::Text("%.1f FPS   /   %.2f ms", fps, fps > 0.0f ? 1000.0f / fps : 0.0f);
                ImGui::TextDisabled("主视口 · 最近渲染帧%s", stats.bSceneReused ? "（复用缓存）" : "");
                ImGui::Separator();
                if (ImGui::BeginTable("##RenderStats", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
                {
                    const auto row = [](const char* label, auto value)
                    {
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
                        ImGui::TableNextColumn(); const auto text = std::to_string(value); ImGui::TextUnformatted(text.c_str());
                    };
                    row("渲染宽度", Application::GetInstance().GetRenderWidth());
                    row("渲染高度", Application::GetInstance().GetRenderHeight());
                    row("不透明 Draw Calls", stats.StaticOpaqueDrawCount);
                    row("透明 Draw Calls", stats.CpuTransparentDrawCount);
                    row("骨骼 Draw Calls", stats.SkinnedDrawCount);
                    row("深度预通道 Draw Calls", stats.DepthPrepassDrawCount);
                    row("方向光阴影 Draw Calls", stats.DirectionalShadowDrawCount);
                    row("GPU 间接绘制命令", stats.GpuIndirectDrawCount);
                    row("2D 批次", stats.Primitive2DBatchCount);
                    row("输入渲染对象", stats.InputRenderObjects);
                    row("LOD 后候选三角形", stats.SelectedRasterTriangleCount);
                    row("材质程序", stats.MaterialProgramCount);
                    row("纹理绑定", stats.BindlessTextureCount);
                    row("RenderGraph Pass", stats.CompiledPasses.size());
                    row("几何缓冲使用 / MiB", stats.GeometryArenaUsedBytes / (1024 * 1024));
                    ImGui::EndTable();
                }
                ImGui::Spacing();
                ImGui::Text("CPU 准备 %.2f ms / 录制 %.2f ms", stats.CpuFramePreparationMilliseconds, stats.CpuGraphRecordMilliseconds);
                if (stats.bGpuTimingEnabled && stats.GpuRenderGraphSampleCount > 0)
                    ImGui::Text("GPU RenderGraph %.2f ms", stats.GpuRenderGraphLastMilliseconds);
                else ImGui::TextDisabled("GPU 耗时：未采样");
                if (stats.GpuCullingReadbackSubmission > 0)
                    ImGui::Text("GPU 可见 %u / 裁剪 %u", stats.GpuVisibleObjectCount, stats.GpuCulledObjectCount);
                ImGui::TextWrapped("绘制数按通道分别统计，不含 UI 与特效；GPU 数据来自最近完成的回读。");
                if (ImGui::Button("RenderGraph 诊断")) bShowRenderGraphDiagnostics = true;
            }
			ImGui::End();
		}
		bViewportHovered = false;
		bHandleEvent = false;
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

			bViewportHovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(
				ImVec2(mViewportBounds[0].x, mViewportBounds[0].y), ImVec2(mViewportBounds[1].x, mViewportBounds[1].y));
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
            const auto debugMode = RenderSettingsService::GetInstance().GetSnapshot().Debug.PbrDebugMode;
            if (debugMode != 0)
            {
                static constexpr const char* names[] = {"正常着色", "Base Color", "World Normal", "Metallic", "Roughness", "Material AO", "Direct Lighting", "Diffuse IBL", "Specular IBL", "Shadow", "Final HDR", "HDR Validity", "Screen AO", "Depth (log)"};
                const char* name = debugMode < std::size(names) ? names[debugMode] : "Debug";
                ImGui::GetWindowDrawList()->AddText(ImVec2(mViewportBounds[0].x + ImGuiWrapper::S(12), mViewportBounds[0].y + ImGuiWrapper::S(58)), IM_COL32(245, 200, 120, 255), name);
            }
			if (ModeManager::IsEditState())
			{
			const ImVec2 cursorAfterViewport = ImGui::GetCursorPos();
			ImGui::SetCursorScreenPos(ImVec2(mViewportBounds[0].x + ImGuiWrapper::S(12.0f), mViewportBounds[0].y + ImGuiWrapper::S(12.0f)));
			ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ImGuiWrapper::S(3.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, ImGuiWrapper::S(1.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGuiWrapper::S(6.0f, 4.0f));
			ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.075f, 0.085f, 0.10f, 0.94f));
			ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.24f, 0.28f, 0.34f, 0.92f));
			if (ImGui::BeginChild("##ViewportControls", ImVec2(ImMin(ImGuiWrapper::S(viewportPanelSize.x < ImGuiWrapper::S(720.0f) ? 370.0f : 310.0f), ImMax(1.0f, viewportPanelSize.x - ImGuiWrapper::S(24.0f))), ImGuiWrapper::S(40.0f)), true,
				ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
			{
				UI_ViewportMode();
				ImGui::SameLine();
				if (ImGui::Button(EditorLayer::GetText("Show"), ImGuiWrapper::S(64.0f, 0.0f)))
					ImGui::OpenPopup("ViewportShowMenu");
				if (ImGui::BeginPopup("ViewportShowMenu"))
				{
                    const auto snapshot = RenderSettingsService::GetInstance().GetSnapshot();
                    ImGuiWrapper::SeparatorWithText("视图模式");
                    const auto modeItem = [&](const char* label, ScenePbrDebugMode mode)
                    {
                        if (ImGui::MenuItem(label, nullptr, snapshot.Debug.PbrDebugMode == static_cast<uint32_t>(mode)))
                        {
                            auto draft = RenderSettingsService::GetInstance().CreateDraft(SettingsLayer::Session);
                            draft.Patch[std::string(RenderSettingIds::PbrDebugMode)] = static_cast<uint32_t>(mode);
                            const auto result = Application::GetInstance().ApplyRenderSettingsDraft(draft, false);
                            if (!result.bSuccess) mViewportModeError = result.Message;
                            else { mViewportModeError.clear(); ++mEditorRenderRevision; }
                        }
                    };
                    ImGui::BeginDisabled(!ModeManager::b3DMode || !Application::GetInstance().GetActiveOfflineViewportRendererId().empty());
                    modeItem("正常着色", ScenePbrDebugMode::Disabled);
                    if (ImGui::BeginMenu("缓冲区可视化"))
                    {
                        modeItem("基础色 / Base Color", ScenePbrDebugMode::BaseColor);
                        modeItem("世界法线 / World Normal", ScenePbrDebugMode::WorldNormal);
                        modeItem("金属度 / Metallic", ScenePbrDebugMode::Metallic);
                        modeItem("粗糙度 / Roughness", ScenePbrDebugMode::Roughness);
                        modeItem("材质遮蔽 / Material AO", ScenePbrDebugMode::AmbientOcclusion);
                        modeItem("屏幕空间遮蔽 / Screen AO", ScenePbrDebugMode::ScreenSpaceAo);
                        modeItem("深度 / Depth（对数）", ScenePbrDebugMode::ViewDepth);
                        ImGui::EndMenu();
                    }
                    if (ImGui::BeginMenu("光照诊断"))
                    {
                        modeItem("直接光照", ScenePbrDebugMode::DirectLighting);
                        modeItem("漫反射环境光", ScenePbrDebugMode::DiffuseIbl);
                        modeItem("镜面环境光", ScenePbrDebugMode::SpecularIbl);
                        modeItem("阴影", ScenePbrDebugMode::Shadow);
                        modeItem("HDR 有效性", ScenePbrDebugMode::HdrValidity);
                        ImGui::EndMenu();
                    }
                    ImGui::EndDisabled();
                    ImGuiWrapper::SeparatorWithText("显示选项");
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
				if (viewportPanelSize.x < ImGuiWrapper::S(720.0f))
				{
					if (ImGui::Button(GetText("Tools"))) ImGui::OpenPopup("##TransformTools");
					if (ImGui::BeginPopup("##TransformTools"))
					{
						UI_ViewportTransformTools();
						ImGui::EndPopup();
					}
					ImGui::SameLine();
				}
				ImGui::SetNextItemWidth(ImGuiWrapper::S(85.0f));
				ImGui::SliderFloat("##ViewportCameraSpeed", &mEditorCamera.mCameraSpeed, 0.1f, 100.0f, "%.1fx", ImGuiSliderFlags_Logarithmic);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", GetText("Camera Speed"));
			}
			ImGui::EndChild();
			if (viewportPanelSize.x >= ImGuiWrapper::S(720.0f))
			{
				ImGui::SetCursorScreenPos(ImVec2(mViewportBounds[1].x - ImGuiWrapper::S(292.0f), mViewportBounds[0].y + ImGuiWrapper::S(12.0f)));
				if (ImGui::BeginChild("##ViewportTransformTools", ImGuiWrapper::S(280.0f, 40.0f), true,
					ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) UI_ViewportTransformTools();
				ImGui::EndChild();
			}
			ImGui::PopStyleColor(2);
			ImGui::PopStyleVar(3);
			ImGui::SetCursorPos(cursorAfterViewport);
			ImGui::Dummy(ImVec2(0.0f, 0.0f));
			}
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
					RequestOpenScene(ConfigManager::GetFullPath(scenePath));
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

			ImGui::End();
			ImGui::PopStyleVar();
		}
		// ----Windows End----

		// ----Help Begin----
		ImGuiWindowFlags helpMenuFlags = ImGuiWindowFlags_NoDocking;
		if (bShowTutorial)
		{
			const std::string tutorialWindowName = std::string(EditorLayer::GetText("Tutorial")) + "###Tutorial";
            ImGuiWrapper::ConstrainNextWindowToWorkArea(tutorialWindowName.c_str(), ImVec2(540, 340), ImVec2(360, 260));
            if (ImGui::Begin(tutorialWindowName.c_str(), &bShowTutorial, helpMenuFlags))
            {
                ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "HEngine / 入门指南");
                ImGui::TextDisabled("教程待定 · 以下为最简工作流程");
                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                ImGui::TextWrapped("%s", EditorShortcutLabel("1. 从内容浏览器打开场景，在层级面板选择对象。\n\n2. 在属性面板编辑对象；使用视口工具调整变换。\n\n3. 保存场景（Ctrl+S），点击运行预览效果。\n\n4. 停止运行后，打开发布面板（Ctrl+P）检查环境并构建。").c_str());
                ImGui::Spacing();
                ImGui::TextWrapped("可以先从“帮助 → 示例 → 材质展示场景”开始探索。");
            }
			ImGui::End();
		}
		if (bShowAboutMe)
		{
			const std::string aboutWindowName = std::string(EditorLayer::GetText("About Me")) + "###About Me";
            ImGuiWrapper::ConstrainNextWindowToWorkArea(aboutWindowName.c_str(), ImVec2(570, 350), ImVec2(380, 280));
            if (ImGui::Begin(aboutWindowName.c_str(), &bShowAboutMe, helpMenuFlags))
            {
                ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "HEngine");
                ImGui::TextDisabled("由热爱驱动，与社区共建");
                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                ImGui::TextWrapped("你好，我是 hbh，HEngine 的开发者。本科毕业于清华大学自动化系，曾在大型游戏公司从事多年游戏引擎研发工作。");
                ImGui::Spacing();
                ImGui::TextWrapped("欢迎为 HEngine 点亮 Star，也欢迎通过 Issues 与 Pull Requests 分享想法、反馈问题、贡献代码。期待与你一起，让这个引擎不断成长。");
                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                ImGui::TextUnformatted("github.com/hebohang/HEngine");
                if (ImGui::Button("访问 GitHub 仓库"))
                {
                    if (!FileDialogs::OpenWebUrl("https://github.com/hebohang/HEngine"))
                        ImGui::SetClipboardText("https://github.com/hebohang/HEngine");
                }
                ImGuiWrapper::SameLine(0.0f);
                if (ImGui::Button("复制仓库地址")) ImGui::SetClipboardText("https://github.com/hebohang/HEngine");
            }
			ImGui::End();
		}
		if (bShowDemoImGui)
		{
			ImGui::ShowDemoWindow(&bShowDemoImGui);
		}
		// ----Help End----

		UI_Toolbar();
		UpdateRuntimeCursorPolicy();
		UI_StatusBar();
		if (mBuildPublishPanel) mBuildPublishPanel->Draw();

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

	void EditorLayer::UI_ViewportMode()
	{
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

	}

	void EditorLayer::UI_ViewportTransformTools()
	{
		IconManager& iconManager = IconManager::GetInstance();
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

	}

	void EditorLayer::UI_Toolbar()
	{
		const float menuHeight = ImGui::GetFrameHeight();
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGuiWrapper::S(6.0f, 6.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImGuiWrapper::S(4.0f, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImGuiWrapper::S(6.0f, 6.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGuiWrapper::S(3.0f));
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.28f, 0.52f, 0.86f, 0.24f));
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.20f, 0.23f, 0.28f, 0.80f));

		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + menuHeight));
		ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, ImGuiWrapper::S(44.0f)));
		ImGui::SetNextWindowDockID(0);
		ImGui::Begin("##toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		ImGui::SetCursorPosY(ImGuiWrapper::S(6.0f));

		IconManager& iconManager = IconManager::GetInstance();
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
		drawToolbarButton("SaveIcon", EditorShortcutLabel(EditorLayer::GetText("Save Scene (Ctrl+S)")).c_str(), ImVec4(1, 1, 1, 1), &bSavePressed);
		if (bSavePressed)
			SaveScene();

		ImGui::SameLine();
		ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
		ImGui::SameLine();

		if (!bCanUndo)
			ImGui::BeginDisabled();
		bool bUndoPressed = false;
		const std::string undoTooltip = MakeUndoTooltip(EditorShortcutLabel(EditorLayer::GetText("Undo (Ctrl+Z)")).c_str(), undoStepLabel);
		drawToolbarButton("UndoIcon", undoTooltip.c_str(), bCanUndo ? ImVec4(1, 1, 1, 1) : ImVec4(0.7f, 0.7f, 0.7f, 0.8f), &bUndoPressed);
		if (!bCanUndo)
			ImGui::EndDisabled();
		if (bUndoPressed)
			UndoActiveDocument();

		ImGui::SameLine();
		if (!bCanRedo)
			ImGui::BeginDisabled();
		bool bRedoPressed = false;
		const std::string redoTooltip = MakeUndoTooltip(EditorShortcutLabel(EditorLayer::GetText("Redo (Ctrl+Y / Ctrl+Shift+Z)")).c_str(), redoStepLabel);
		drawToolbarButton("RedoIcon", redoTooltip.c_str(), bCanRedo ? ImVec4(1, 1, 1, 1) : ImVec4(0.7f, 0.7f, 0.7f, 0.8f), &bRedoPressed);
		if (!bCanRedo)
			ImGui::EndDisabled();
		if (bRedoPressed)
			RedoActiveDocument();

		const float buttonWidth = toolbarIconSize.x + ImGui::GetStyle().FramePadding.x * 2.0f;
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const float transportWidth = buttonWidth * 3.0f + spacing * 2.0f;
		// Align the transport group with the actual viewport window, including dock resizing.
		// Window positions are already in pixel space; only design spacing is DPI-scaled.
		const ImVec2 toolbarPosition = ImGui::GetWindowPos();
		float transportCenter = ImGui::GetWindowWidth() * 0.5f;
		if (const ImGuiWindow* viewportWindow = ImGui::FindWindowByName("###Viewport");
			bShowViewport && viewportWindow && viewportWindow->Active && !viewportWindow->Hidden)
			transportCenter = viewportWindow->Pos.x + viewportWindow->Size.x * 0.5f - toolbarPosition.x;
		const float minimumTransportX = ImGui::GetItemRectMax().x - toolbarPosition.x + ImGuiWrapper::S(16.0f);
		const float maximumTransportX = ImMax(minimumTransportX, ImGui::GetWindowWidth() - transportWidth - ImGuiWrapper::S(12.0f));
		const float transportX = ImClamp(transportCenter - transportWidth * 0.5f, minimumTransportX, maximumTransportX);
		ImGui::SameLine(transportX);
		const ImVec2 transportMin = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddRectFilled(
			transportMin, ImVec2(transportMin.x + transportWidth, transportMin.y + buttonWidth),
			ImGui::GetColorU32(ImVec4(0.075f, 0.085f, 0.10f, 1.0f)), ImGuiWrapper::S(3.0f));
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

		// Keep all transport slots stable across Edit / Play / Pause.
		auto transportButton = [&](const char* id, bool bStep, bool bResume, const char* tooltip)
		{
			ImGui::SameLine();
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			const bool bPressed = ImGui::Button(id, ImVec2(buttonWidth, buttonWidth));
			ImDrawList* draw = ImGui::GetWindowDrawList();
			const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
			const ImVec2 center(origin.x + buttonWidth * 0.5f, origin.y + buttonWidth * 0.5f);
			const float half = ImGuiWrapper::S(6.0f);
			if (bStep || bResume)
				draw->AddTriangleFilled(ImVec2(center.x - half, center.y - half), ImVec2(center.x - half, center.y + half), ImVec2(center.x + half, center.y), color);
			else
				draw->AddRectFilled(ImVec2(center.x - half, center.y - half), ImVec2(center.x - half * 0.35f, center.y + half), color);
			if (bStep || !bResume)
				draw->AddRectFilled(ImVec2(center.x + half * 0.5f, center.y - half), ImVec2(center.x + half, center.y + half), color);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tooltip);
			return bPressed;
		};
		ImGui::BeginDisabled(ModeManager::IsEditState());
		if (transportButton("##PauseResume", false, mScriptPlay.IsPaused(), GetText(mScriptPlay.IsPaused() ? "Resume" : "Pause")))
		{
			if (mScriptPlay.IsPaused()) mScriptPlay.Resume(); else mScriptPlay.Pause();
			if (auto* scripts = mActiveScene->TryGetSystem<ScriptSystem>()) scripts->SetPaused(mScriptPlay.IsPaused());
			UpdateRuntimeCursorPolicy();
		}
		ImGui::BeginDisabled(mScriptPlay.State() != ScriptPlayState::Paused);
		if (transportButton("##Step", true, false, GetText("Step"))) mScriptPlay.Step();
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		if (!ModeManager::IsEditState())
		{
			const char* label = GetText("Play Options");
			const float rightWidth = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
			ImGui::SameLine(ImMax(transportX + transportWidth + spacing, ImGui::GetWindowWidth() - rightWidth - ImGuiWrapper::S(12.0f)));
			if (ImGui::Button(label)) ImGui::OpenPopup("##PlayOptions");
			if (ImGui::BeginPopup("##PlayOptions"))
			{
				ImGui::TextDisabled("%s", GetText("Release Mouse: Shift+F1 / Alt"));
				if (ImGui::MenuItem(GetText("Apply Play Values")))
				{
					try
					{
						mSceneEditing->SetPlaying(true);
						mSceneEditing->ApplyPlayScriptFields(SceneSerializer(mActiveScene).SerializeToJson());
						mSceneDirty = mSceneEditing->IsDirty();
					}
					catch (const std::exception& error) { HE_CORE_ERROR("Apply Play Values failed: {0}", error.what()); }
				}
				ImGui::EndPopup();
			}
		}

		ImGui::End();

		ImGui::PopStyleColor(4);
		ImGui::PopStyleVar(4);
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
			const char* modeText = ModeManager::IsEditState() ? GetText("Edit Mode") : GetText(mScriptPlay.IsPaused() ? "Paused" : "Play Mode");
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
		bShowPlugins = defaultLayout.value("ShowPlugins", false);
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
		defaultLayout["ShowPlugins"] = bShowPlugins;
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
        // Editor lifecycle shortcuts must remain reachable while a game's UI
        // owns keyboard focus or its camera captures the pointer.
        EventDispatcher lifecycleDispatcher(e);
        lifecycleDispatcher.Dispatch<KeyPressedEvent>([this](KeyPressedEvent& key)
        {
            if (key.GetRepeatCount() > 0) return false;
            if (key.GetKeyCode() == Key::F8 && !ModeManager::IsEditState())
            {
                TryChangeSceneMode(false);
                return true;
            }
            if (key.GetKeyCode() == Key::F6 && ModeManager::IsEditState())
            {
                TryChangeSceneMode(true);
                return true;
            }
            return false;
        });
        if (e.Handled) return;
		mIteratingPanelMap = true;
		for (const auto& [panelTypeName, panel] : mPanelMap)
		{
			panel->OnEvent(e);
		}
		mIteratingPanelMap = false;
		FlushPendingMaterialPanels();
		FlushPendingAssetPreviewPanels();

		UpdateRuntimeCursorPolicy();
		if (!ModeManager::IsEditState() && ScriptInputContext::Region.Enabled && e.GetEventType() == EventType::MouseButtonPressed)
		{
			mRuntimeInputEngaged = true;
			UpdateRuntimeCursorPolicy();
		}
		if (!ModeManager::IsEditState() && ScriptInputContext::Region.KeyboardEnabled)
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
#if defined(__APPLE__)
        control = control || Input::IsKeyPressed(Key::LeftSuper) || Input::IsKeyPressed(Key::RightSuper);
#endif
        bool shift = Input::IsKeyPressed(Key::LeftShift) || Input::IsKeyPressed(Key::RightShift);
		bool mouseRight = Input::IsMouseButtonPressed(Mouse::ButtonRight);

		if (!ModeManager::IsEditState())
		{

            if (control && e.GetKeyCode() == Key::O)
            {
                OpenScene();
                return true;
            }
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
					OpenBuildPublish();
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
        if (mSceneMailbox.IsVfxDiagnosticActive()) return;
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

	void EditorLayer::RequestOpenScene(const std::filesystem::path& path)
	{
		// Asset activation happens during ImGui construction. Replace the scene
		// at the next authoring boundary, before any panel reads the new scene.
		if (!mScriptPlay.IsTransitioning())
			mPendingSceneOpenPath = path;
	}

    std::string EditorLayer::ValidateAssetMove() const
    {
        if (!ModeManager::IsEditState() || mScriptPlay.IsTransitioning()) return "Stop Play before moving assets.";
        if (mSceneDirty || !mPendingSceneHistoryLabel.empty() || mSceneHistoryInteractivePending || !mPendingSceneEdits.empty())
            return "Save the current scene before moving assets so its references can be updated safely.";
        if (!mPendingSceneOpenPath.empty()) return "Wait for the pending scene change before moving assets.";
        for (const auto& [name, panel] : mPanelMap)
        {
            const auto* material = dynamic_cast<MaterialPanel*>(panel.get());
            const auto* controller = dynamic_cast<AnimControllerPanel*>(panel.get());
            const auto* ui = dynamic_cast<UIPanel*>(panel.get());
            if ((material && material->HasUnsavedChanges()) || (controller && controller->IsUndoContextDirty()) ||
                (ui && ui->IsUndoContextDirty()))
                return "Save or revert unsaved material, animation and UI edits before moving assets (including closed windows).";
            if (panel->IsOpen() && (material || controller || ui || dynamic_cast<AssetPreviewPanel*>(panel.get())))
                return "Close material, animation, UI and asset preview windows before moving assets.";
        }
        return {};
    }

    void EditorLayer::RefreshAfterAssetMove(const std::filesystem::path& source, const std::filesystem::path& target)
    {
        mInvalidateClosedAssetPanels = true;
        if (mEditorScenePath.empty()) return;
        const auto relative = mEditorScenePath.lexically_normal().lexically_relative(source.lexically_normal());
        if (!relative.empty() && *relative.begin() != "..") RequestOpenScene((target / relative).lexically_normal());
        else RequestOpenScene(mEditorScenePath);
    }

    void EditorLayer::OpenScene()
    {
		if (mScriptPlay.IsTransitioning()) return;

		std::filesystem::path filepath = FileDialogs::OpenFile("HEngine Level (*.he)\0*.he\0\0", ConfigManager::GetFullPath("Assets/Scenes").string());
		if (!filepath.empty())
			RequestOpenScene(filepath);
    }

	void EditorLayer::OpenScene(const std::filesystem::path& path)
	{
        if (mSceneMailbox.IsVfxDiagnosticActive())
        {
            mPendingSceneOpenPath = path;
            return;
        }
		if (mScriptPlay.IsTransitioning()) return;

		if (path.extension().string() != ".he")
		{
			HE_WARN("Could not load {0} - not a scene file", path.filename().string());
			return;
		}

		Ref<Level> newScene = CreateRef<Level>();
		SceneSerializer serializer(newScene);
		if (serializer.Deserialize(path.string()))
		{
            // Validate first. Cancellation/malformed scenes leave the current
            // session alive; successful opens cross the ordinary Stop barrier.
            if (!ModeManager::IsEditState()) TryChangeSceneMode(false);
            if (!ModeManager::IsEditState()) return;
			newScene->OnViewportResize((uint32_t)ConfigManager::mViewportSize.x, (uint32_t)ConfigManager::mViewportSize.y);
			EmbreeSystem::ResetSharedContext();
			GetPanel<SceneHierarchyPanel>()->SetContext(newScene);

			mEditorScene = newScene;
			mActiveScene = newScene;
			mEditorScenePath = path;
			InitializeEditorCameraFromPrimary(mEditorCamera, newScene);
			ResetSceneHistory("Open Scene");
			HE_CORE_INFO("Scene opened: {0}; dirty={1}", path.generic_string(), mSceneDirty);

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
		}
    }

	void EditorLayer::SerializeScene(Ref<Level> scene, const std::filesystem::path& path)
	{
		if (!ModeManager::IsEditState() || scene != mEditorScene || !mSceneEditing) return;
		(void)QueueSceneEditingRequest("save", {{"sceneId",mSceneEditing->GetSceneId()},
			{"baseRevision",mSceneEditing->GetRevision()},{"requestId",UUID().str()},
			{"path",Utils::NormalizeAssetPath(ConfigManager::GetRelativePath(path))}});
		if (!mPendingSceneEdits.empty()) mPendingSceneEdits.back().LocalUi = true;
	}

	void EditorLayer::OpenBuildPublish()
	{
		if (!mBuildPublishPanel) mBuildPublishPanel = CreateScope<BuildPublishPanel>();
		mBuildPublishPanel->Open();
	}

	void EditorLayer::OnScenePlay()
	{
		if (mScriptPlay.IsTransitioning()) throw std::logic_error("A scene transition is already in progress.");
		if (!ModeManager::IsEditState())
			return;
		if (!mEditorScene)
			throw std::logic_error("Cannot enter Play mode without an editor scene.");
		if (!PrepareViewportModeForPlay())
			return;

        auto backend = ScriptBackendRegistry::Find("he.script.csharp");
        if (auto* reloader = dynamic_cast<IScriptReloader*>(backend.get()))
            reloader->Reload(mPendingScriptGeneration);
        mPendingScriptGeneration.clear();
        mPrePlayEditorState = GetPanel<SceneHierarchyPanel>()->ExportEditorState();
		Application::GetInstance().ClearRuntimeSceneRequest();
		mRuntimeScenePath = mEditorScenePath;
		Ref<Level> runtimeScene = Level::Copy(mEditorScene, true);
        mScriptPlay.BeginPlay();
		try
		{
            InputActions::Get().ReloadProjectMap();
			runtimeScene->OnRuntimeStart();
			EmbreeSystem::ResetSharedContext();
			if (auto* physicsSystem = mEditorScene->TryGetSystem<PhysicSystem3D>())
				physicsSystem->ReleaseEditorDebugResources();

			mActiveScene = runtimeScene;
			GetPanel<SceneHierarchyPanel>()->SetContext(mActiveScene);
			ModeManager::ChangeState();

			mRuntimeInputEngaged = true;
			CenterCursorToViewport();
			UpdateRuntimeCursorPolicy();
			Application::GetWindow().SetInputMethodEnabled(false);
			(void)Application::GetInstance().GetProjectResourceWarmup().QueueGroup("Boot");
            mScriptPlay.CommitPlay();
		}
		catch (...)
		{
            mScriptPlay.AbortPlay();
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
				mRuntimeInputEngaged = false;
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
		if (mScriptPlay.IsTransitioning()) throw std::logic_error("A scene transition is already in progress.");
		if (ModeManager::IsEditState())
			return;
        mScriptPlay.BeginStop();
        InputActions::Get().Suspend();

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
			GetPanel<SceneHierarchyPanel>()->ImportEditorState(mPrePlayEditorState);
		}
		catch (...)
		{
			recordFailure("Editor scene context restoration encountered an additional failure.");
		}

		mRuntimeInputEngaged = false;
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

        mScriptPlay.CommitStop();
		UpdateRuntimeCursorPolicy();
		Application::GetInstance().ClearRuntimeSceneRequest();
		mRuntimeScenePath.clear();
		if (firstFailure)
			std::rethrow_exception(firstFailure);
	}

	void EditorLayer::ProcessRuntimeSceneRequest()
	{
		if (ModeManager::IsEditState() || mScriptPlay.IsTransitioning()) return;
		bool bForceReload = false;
		auto request = Application::GetInstance().ConsumeRuntimeSceneRequest(&bForceReload);
		if (!request) return;
		try
		{
			const auto document = ResolveSceneDocumentPath(
				ConfigManager::GetInstance().GetRootFolder(), *request);
			if (!bForceReload && document.FullPath == mRuntimeScenePath) return;
			// Only replace the runtime copy, at a frame boundary outside script callbacks.
			// The author scene, selection and undo history remain available to Stop.
			auto nextScene = CreateRef<Level>();
			SceneSerializer serializer(nextScene);
			if (!serializer.Deserialize(document.FullPath.string()))
				throw std::runtime_error("Runtime scene could not be deserialized: " + *request);
			nextScene->OnViewportResize(static_cast<uint32_t>(ConfigManager::mViewportSize.x),
				static_cast<uint32_t>(ConfigManager::mViewportSize.y));
			mActiveScene->OnRuntimeStop();
			mActiveScene = std::move(nextScene);
			GetPanel<SceneHierarchyPanel>()->SetContext(mActiveScene);
			EmbreeSystem::ResetSharedContext();
			mActiveScene->OnRuntimeStart();
			mRuntimeScenePath = document.FullPath;
			HE_CORE_INFO("[Editor] Runtime scene transition complete scene={0}", *request);
		}
		catch (...)
		{
			ReportEditorCleanupFailure("Editor runtime scene transition failed; returning to Edit.", std::current_exception());
			TryChangeSceneMode(false);
		}
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
		ImGuiIO& io = ImGui::GetIO();
		const bool bPlaying = !ModeManager::IsEditState();
		const auto mainPosition = ImGui::GetMainViewport()->Pos;
		const auto mouse = Input::GetMousePosition();
		const bool bInside = bShowViewport && mouse.x + mainPosition.x >= mViewportBounds[0].x &&
			mouse.y + mainPosition.y >= mViewportBounds[0].y && mouse.x + mainPosition.x < mViewportBounds[1].x &&
			mouse.y + mainPosition.y < mViewportBounds[1].y;
		ImGuiWindow* hoveredWindow = nullptr;
		ImGuiWindow* hoveredUnderMovingWindow = nullptr;
		ImGui::FindHoveredWindowEx(ImVec2(mouse.x + mainPosition.x, mouse.y + mainPosition.y), false,
			&hoveredWindow, &hoveredUnderMovingWindow);
		const bool bViewportPointer = bInside && hoveredWindow && hoveredWindow->ID == ImHashStr("Viewport") &&
			!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
		const bool bRelease = Input::IsKeyPressed(Key::LeftAlt) || Input::IsKeyPressed(Key::RightAlt) ||
			((Input::IsKeyPressed(Key::LeftShift) || Input::IsKeyPressed(Key::RightShift)) && Input::IsKeyPressed(Key::F1));
		if (!bPlaying || io.AppFocusLost || !bShowViewport || mScriptPlay.IsPaused() || bRelease ||
			(!mRuntimeCursorCaptured && !bViewportPointer))
			mRuntimeInputEngaged = false;

		auto& rmlUi = RmlUiManager::GetInstance();
		const bool bUiCursorVisible = bPlaying && rmlUi.IsMouseCursorVisibleRequested();
		auto camera = mActiveScene ? mActiveScene->GetPrimaryCameraEntity() : Entity{};
		const bool bOrthographic = camera && camera.HasComponent<CameraComponent>() &&
			camera.GetComponent<CameraComponent>().Camera.GetProjectionType() == SceneCamera::ProjectionType::Orthographic;
		SetRuntimeCursorCaptured(bPlaying && mRuntimeInputEngaged && !bUiCursorVisible && !bOrthographic);
		const bool bViewportInput = bPlaying && !io.AppFocusLost && !mScriptPlay.IsPaused() && !bRelease &&
			(mRuntimeCursorCaptured || bViewportPointer);
		ScriptInputContext::Region = {
			true, bViewportInput, bViewportInput && !io.WantTextInput,
			mViewportBounds[0].x - mainPosition.x, mViewportBounds[0].y - mainPosition.y,
			mViewportBounds[1].x - mViewportBounds[0].x, mViewportBounds[1].y - mViewportBounds[0].y};
		// RmlUi must not replace ImGui's native cursor outside the game image.
		rmlUi.SetMouseCursorEnabled(bViewportInput && bUiCursorVisible);
		if (bViewportInput && bUiCursorVisible) io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
		else io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
		if (mRuntimeCursorCaptured) io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
		else io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
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
			// ImGui uses client coordinates with native multi-viewports disabled.
			const ImVec2 mainPosition = ImGui::GetMainViewport()->Pos;

			const float centerScreenX = mViewportBounds[0].x + viewportWidth * 0.5f;
			const float centerScreenY = mViewportBounds[0].y + viewportHeight * 0.5f;
			cursorX = centerScreenX - mainPosition.x;
			cursorY = centerScreenY - mainPosition.y;
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
		if (!ModeManager::IsEditState() || !mEditorScene || !mSceneEditing) return;
		// Existing widgets report the end of their interaction. Commit at next
		// OnPreUpdate, after UI construction and before rendering submission.
		mSceneHistoryInteractivePending = bInteractive;
		mPendingSceneHistoryLabel = label ? label : "Edit Scene";
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



	void EditorLayer::ResetSceneHistory(const char* label)
	{
		if (!mEditorScene) return;
		mSceneEditing = CreateScope<SceneEditingService>();
		const std::string path = mEditorScenePath.empty() ? "__untitled__.he" :
			Utils::NormalizeAssetPath(ConfigManager::GetRelativePath(mEditorScenePath));
		mSceneEditing->Bind(ConfigManager::GetInstance().GetRootFolder(), path,
			SceneSerializer(mEditorScene).SerializeToJson(), [this](const Json& values) {
				ApplySceneEditingValues(mEditorScene, values);
				mEditorScene->OnViewportResize((uint32_t)ConfigManager::mViewportSize.x, (uint32_t)ConfigManager::mViewportSize.y);
			}, !mEditorScenePath.empty());
		mSceneEditing->SetChangeObserver([this](uint64_t) {
			++mEditorRenderRevision; mEditorWarmupFramesRemaining = 10;
		});
		mSceneHistoryInteractivePending = false; mPendingSceneHistoryLabel.clear();
		mSceneObservationHandles.clear(); mSceneObservationSeen.clear(); mSceneObservationIndex = 0;
		mNextSceneObservation = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
		mPendingSceneEdits.clear(); mSceneEditResults.clear();
		mSceneDirty = mSceneEditing->IsDirty();
		mEditorWarmupFramesRemaining = 30; ++mEditorRenderRevision;
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
	{ return ModeManager::IsEditState() && mSceneEditing && mSceneEditing->CanUndo(); }
	bool EditorLayer::CanRedo() const
	{ return ModeManager::IsEditState() && mSceneEditing && mSceneEditing->CanRedo(); }
	bool EditorLayer::Undo()
	{
		if (!CanUndo()) return false;
		QueueSceneEditingRequest("undo", {{"sceneId",mSceneEditing->GetSceneId()},
			{"baseRevision",mSceneEditing->GetRevision()},{"requestId",UUID().str()}});
		if (!mPendingSceneEdits.empty()) mPendingSceneEdits.back().LocalUi = true;
		return true;
	}
	bool EditorLayer::Redo()
	{
		if (!CanRedo()) return false;
		QueueSceneEditingRequest("redo", {{"sceneId",mSceneEditing->GetSceneId()},
			{"baseRevision",mSceneEditing->GetRevision()},{"requestId",UUID().str()}});
		if (!mPendingSceneEdits.empty()) mPendingSceneEdits.back().LocalUi = true;
		return true;
	}
	Json EditorLayer::QueueSceneEditingRequest(const std::string& method, const Json& request)
	{
		if (method == "result")
		{
			const std::string ticket = request.at("ticket");
			for (const auto& pending : mPendingSceneEdits)
				if (pending.Ticket == ticket) return {{"ok",true},{"ticket",ticket},{"completion","queued"}};
			const auto found = mSceneEditResults.find(ticket);
			return found != mSceneEditResults.end() ? found->second : Json{{"ok",false},{"error",{{"code","ticket_expired"}}}};
		}
		if (mPendingSceneEdits.size() >= 32) return {{"ok",false},{"error",{{"code","queue_full"}}}};
		const std::string ticket = UUID().str();
		mPendingSceneEdits.push_back({ticket,method,request});
		return {{"ok",true},{"sceneId",mSceneEditing ? mSceneEditing->GetSceneId() : ""},{"ticket",ticket},{"completion","queued"}};
	}
	void EditorLayer::QueueSceneOperations(Json operations)
	{
		if (!mSceneEditing || !ModeManager::IsEditState()) return;
		if (!mPendingSceneEdits.empty() && mPendingSceneEdits.back().LocalUi && mPendingSceneEdits.back().Method == "apply")
		{
			for (auto& operation : operations) mPendingSceneEdits.back().Request["operations"].push_back(std::move(operation));
			return;
		}
		(void)QueueSceneEditingRequest("apply", {{"sceneId",mSceneEditing->GetSceneId()},
			{"baseRevision",mSceneEditing->GetRevision()},{"requestId",UUID().str()},{"operations",std::move(operations)}});
		if (!mPendingSceneEdits.empty()) mPendingSceneEdits.back().LocalUi = true;
	}

	bool EditorLayer::IsUndoContextDirty() const
	{
		return ModeManager::IsEditState() && mSceneDirty;
	}
}
