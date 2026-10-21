#include "Editor/ImGuiWrapper/ImGuiWrapper.h"
#include "Editor/EditorLayer.h"
#include "Editor/Panels/SceneHierarchyPanel.h"
#include "Editor/Panels/ScriptInspector.h"
#include "Editor/Panels/SequenceInspector.h"
#include "Editor/Manager/IconManager.h"
#include "Runtime/Profile/Profile.h"
#include "Runtime/Scripting/ScriptEngine.h"
#include "Runtime/Scripting/ScriptFieldsJson.h"
#include "Runtime/Scripting/ScriptBackendRegistry.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/Component/Basic/PrefabInstanceComponent.h"
#include "Runtime/EcsFramework/Serializer/PrefabSerializer.h"
#include "Runtime/Graphics/Material/MaterialResourceCache.h"
#include "Runtime/Graphics/Lighting/PhotometricUnits.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "Runtime/Utils/PlatformUtils.h"
#include "Runtime/Utils/StringUtils/StringUtils.h"
#include "_generated/serializer/engine/Runtime/EcsFramework/Component/Script/ScriptComponent.gen.h"

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <magic_enum.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cfloat>
#include <cctype>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>


namespace HEngine
{
	static Entity sSelectionContext;
    static std::vector<Entity> sScriptSelection;
	static SceneHierarchyPanel::MeshSelectionContext sMeshSelectionContext;
	static Entity sRenameContext;
	static char sRenameBuffer[256] = {};
	static ImGuiTextFilter sHierarchyFilter;

	namespace
	{
		constexpr const char* kEntityPayloadType = "SCENE_ENTITY_UUID";

		struct EntityUiState
		{
			bool bVisible = true;
			bool bLocked = false;
			bool bExpanded = true;
		};

		using EntityUiStateMap = std::unordered_map<UUID, EntityUiState>;
		static std::unordered_map<const Level*, EntityUiStateMap> sEntityUiStates;

		static EntityUiState* FindEntityUiState(Entity entity)
		{
			if (!entity)
				return nullptr;

			const Level* level = entity.GetLevel();
			auto levelIt = sEntityUiStates.find(level);
			if (levelIt == sEntityUiStates.end())
				return nullptr;

			auto stateIt = levelIt->second.find(entity.UUID());
			if (stateIt == levelIt->second.end())
				return nullptr;

			return &stateIt->second;
		}

		static EntityUiState& EnsureEntityUiState(Entity entity)
		{
			HE_CORE_ASSERT(entity);
			return sEntityUiStates[entity.GetLevel()][entity.UUID()];
		}

		static bool GetEntityVisibleState(Entity entity)
		{
			if (entity && entity.HasComponent<EditorEntityStateComponent>())
				return entity.GetComponent<EditorEntityStateComponent>().bVisible;
			return true;
		}

		static void SetEntityVisibleState(Entity entity, bool bVisible)
		{
			if (!entity)
				return;
			entity.AddOrReplaceComponent<EditorEntityStateComponent>(
				entity.HasComponent<EditorEntityStateComponent>()
					? entity.GetComponent<EditorEntityStateComponent>()
					: EditorEntityStateComponent{}).bVisible = bVisible;
			entity.GetLevel()->InvalidateEditorEntityStateCache();
		}

		static bool GetEntityLockedState(Entity entity)
		{
			if (entity && entity.HasComponent<EditorEntityStateComponent>())
				return entity.GetComponent<EditorEntityStateComponent>().bLocked;
			return false;
		}

		static void SetEntityLockedState(Entity entity, bool bLocked)
		{
			if (!entity)
				return;
			entity.AddOrReplaceComponent<EditorEntityStateComponent>(
				entity.HasComponent<EditorEntityStateComponent>()
					? entity.GetComponent<EditorEntityStateComponent>()
					: EditorEntityStateComponent{}).bLocked = bLocked;
			entity.GetLevel()->InvalidateEditorEntityStateCache();
		}

		static bool GetEntityExpandedState(Entity entity)
		{
			if (EntityUiState* state = FindEntityUiState(entity))
				return state->bExpanded;
			if (entity && entity.HasComponent<SceneFolderComponent>())
				return entity.GetComponent<SceneFolderComponent>().bExpanded;
			return true;
		}

		static void SetEntityExpandedState(Entity entity, bool bExpanded)
		{
			if (!entity)
				return;
			EnsureEntityUiState(entity).bExpanded = bExpanded;
			if (entity.HasComponent<SceneFolderComponent>())
				entity.GetComponent<SceneFolderComponent>().bExpanded = bExpanded;
		}

		static void RemoveEntityUiState(Entity entity)
		{
			if (!entity)
				return;

			const Level* level = entity.GetLevel();
			auto levelIt = sEntityUiStates.find(level);
			if (levelIt == sEntityUiStates.end())
				return;

			levelIt->second.erase(entity.UUID());
			if (levelIt->second.empty())
				sEntityUiStates.erase(levelIt);
		}

		static void RemoveEntityUiStateRecursive(const Ref<Level>& context, Entity entity)
		{
			if (!context || !entity)
				return;

			for (Entity child : context->GetFolderChildren(entity))
				RemoveEntityUiStateRecursive(context, child);
			for (Entity child : context->GetChildren(entity))
				RemoveEntityUiStateRecursive(context, child);

			RemoveEntityUiState(entity);
		}

		static ImGuiTextureHandle GetIconOrFallback(const std::string& iconName, const std::string& fallbackName)
		{
			IconManager& iconManager = IconManager::GetInstance();
			if (const ImGuiTextureHandle icon = iconManager.TryGet(iconName))
				return icon;
			return iconManager.TryGet(fallbackName);
		}

		static std::string PickEntityIconName(Entity entity)
		{
			if (entity.HasComponent<SceneFolderComponent>())
				return "DirectoryIcon";
			if (entity.HasComponent<PointLightComponent>())
				return "PointLightEntityIcon";
			if (entity.HasComponent<DirectionalLightComponent>())
				return "DirLightEntityIcon";
			if (entity.HasComponent<EnvironmentLightComponent>())
				return "DirLightEntityIcon";
			if (entity.HasComponent<CameraComponent>())
				return "CameraEntityIcon";
			if (entity.HasComponent<SoundComponent>() || entity.HasComponent<AudioListenerComponent>())
				return "AudioEntityIcon";
			if (entity.HasComponent<MeshComponent>())
				return "MeshEntityIcon";
			return "EmptyEntityIcon";
		}

		template <typename UIFunc>
		static void DrawLabeledControl(const char* label, UIFunc&& uiFunc, float labelWidth = ImGuiWrapper::kDefaultPropertyLabelWidth)
		{
			ImGuiWrapper::PropertyRow(EditorLayer::GetText(label), std::forward<UIFunc>(uiFunc), labelWidth);
		}

		static void DrawLightIntensity(float& intensity, int* units = nullptr, float coneAngle = 180.0f)
		{
			if (units)
			{
				DrawLabeledControl("Intensity Units", [&]()
				{
					const int previous = *units;
					if (ImGui::Combo("##IntensityUnits", units, "Unitless\0Candela (cd)\0Lumen (lm)\0"))
						intensity = Photometry::ConvertIntensity(intensity, previous, *units, Photometry::SolidAngle(coneAngle));
				});
			}
			const char* label = !units ? "Intensity (lux)" : *units == 1 ? "Intensity (cd)" : *units == 2 ? "Intensity (lm)" : "Intensity (unitless)";
			DrawLabeledControl(label, [&]()
			{
				// Drag range is not a physical ceiling; direct input accepts finite nonnegative values.
				ImGui::DragFloat("##PhotometricIntensity", &intensity, std::max(0.01f, intensity * 0.01f), 0.0f, FLT_MAX, "%.6g");
				intensity = Photometry::NonNegative(intensity);
			});
		}

		static std::string NormalizeBackSlash(std::string path)
		{
			for (char& c : path)
			{
				if (c == '\\')
					c = '/';
			}
			return path;
		}

		static std::string ToRootRelativePath(const std::filesystem::path& payloadPath)
		{
			std::filesystem::path relativePath = payloadPath;
			if (payloadPath.is_absolute())
			{
				relativePath = std::filesystem::relative(payloadPath, ConfigManager::GetInstance().GetRootFolder());
			}
			else
			{
				const std::filesystem::path withAssets = ConfigManager::GetInstance().GetAssetsFolder() / payloadPath;
				if (std::filesystem::exists(withAssets))
				{
					relativePath = std::filesystem::relative(withAssets, ConfigManager::GetInstance().GetRootFolder());
				}
			}

			std::string normalized = NormalizeBackSlash(relativePath.string());
			if (normalized.rfind("Assets/", 0) != 0)
				normalized = "Assets/" + normalized;
			return Utils::NormalizeAssetPath(normalized);
		}

		static bool IsMaterialAssetPath(const std::string& path)
		{
			return std::filesystem::path(path).extension() == ".hmat";
		}

		static bool IsOpenableMaterialPath(const std::string& path)
		{
			if (!IsMaterialAssetPath(path))
				return false;
			return std::filesystem::exists(ConfigManager::GetFullPath(path));
		}

		static bool IsOpenableAnimControllerPath(const std::string& path)
		{
			if (std::filesystem::path(path).extension() != ".hanimctrl")
				return false;
			return std::filesystem::exists(ConfigManager::GetFullPath(path));
		}

		static std::string FileNameFromPath(const std::string& path)
		{
			if (path.empty())
				return "(None)";
			return std::filesystem::path(path).filename().string();
		}

		static bool TryMakeProjectAudioAssetPath(const std::filesystem::path& selectedPath, std::string& outPath)
		{
			outPath.clear();
			if (selectedPath.empty())
				return false;

			std::error_code ec;
			const std::filesystem::path assetsRoot =
				std::filesystem::weakly_canonical(ConfigManager::GetInstance().GetAssetsFolder(), ec);
			if (ec)
				return false;

			const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(selectedPath, ec);
			if (ec || !std::filesystem::is_regular_file(canonicalPath, ec) || ec)
				return false;

			const std::filesystem::path relativePath = std::filesystem::relative(canonicalPath, assetsRoot, ec);
			if (ec || relativePath.empty() || relativePath.is_absolute())
				return false;

			for (const auto& part : relativePath)
			{
				if (part == "..")
					return false;
			}

			std::string extension = canonicalPath.extension().string();
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (extension != ".wav" && extension != ".mp3" && extension != ".flac")
				return false;

			outPath = Utils::NormalizeAssetPath((std::filesystem::path("Assets") / relativePath).generic_string());
			return outPath.rfind("Assets/", 0) == 0;
		}

		static std::string EntityDisplayName(Entity entity)
		{
			if (!entity)
				return "None";

		if (entity.HasComponent<TagComponent>())
				return entity.GetComponent<TagComponent>().Tag;

			return "Entity";
		}

		static Entity GetEditorHierarchyParent(Entity entity)
		{
			if (!entity || !entity.GetLevel())
				return {};
			if (Entity folder = entity.GetLevel()->GetEntityFolder(entity))
				return folder;
			return entity.GetLevel()->GetParent(entity);
		}

		static bool SetEditorHierarchyParent(Entity entity, Entity parent, EditorLayer* editor)
		{
			if (!entity || !entity.GetLevel())
				return false;

			Level* level = entity.GetLevel();
			if (editor && !entity.HasComponent<SceneFolderComponent>() &&
				(!parent || !parent.HasComponent<SceneFolderComponent>()) && !level->GetEntityFolder(entity))
			{
				editor->QueueSceneOperations(Json::array({{{"op","reparent"},{"entity",entity.UUID().str()},
					{"parent",parent ? parent.UUID().str() : ""},{"mode","keepWorld"}}}));
				return true;
			}
			if (parent && parent.HasComponent<SceneFolderComponent>())
				return level->SetEntityFolder(entity, parent);

			if (entity.HasComponent<SceneFolderComponent>())
				return !parent && level->SetEntityFolder(entity, {});

			if (!parent)
			{
				if (level->GetEntityFolder(entity))
					return level->SetEntityFolder(entity, {});
				return level->SetEntityParent(entity, {}, true);
			}

			Entity currentTransformParent = level->GetParent(entity);
			if (currentTransformParent != parent && !level->SetEntityParent(entity, parent, true))
				return false;
			const bool bClearedFolder = level->GetEntityFolder(entity) && level->SetEntityFolder(entity, {});
			return currentTransformParent != parent || bClearedFolder;
		}

		static bool IsEditorHierarchyDescendantOf(Entity entity, Entity potentialAncestor)
		{
			if (!entity || !potentialAncestor || entity == potentialAncestor)
				return false;

			Level* level = entity.GetLevel();
			std::vector<Entity> pending{ entity };
			std::unordered_map<uint32_t, bool> visited;
			for (int depth = 0; !pending.empty() && depth < 512; ++depth)
			{
				Entity current = pending.back();
				pending.pop_back();
				if (!current || !visited.emplace(static_cast<uint32_t>(current), true).second)
					continue;

				Entity folder = level->GetEntityFolder(current);
				Entity transformParent = level->GetParent(current);
				if (folder == potentialAncestor || transformParent == potentialAncestor)
					return true;
				if (folder)
					pending.push_back(folder);
				if (transformParent)
					pending.push_back(transformParent);
			}
			return false;
		}

		static Entity DuplicateEditorHierarchyEntity(Level* level, Entity entity)
		{
			if (!level || !entity)
				return {};

			if (!entity.HasComponent<SceneFolderComponent>())
				return level->DuplicateEntity(entity);

			std::vector<Entity> sources;
			std::unordered_set<uint32_t> visited;
			std::function<void(Entity)> collect = [&](Entity source)
			{
				if (!source || !visited.emplace(static_cast<uint32_t>(source)).second)
					return;
				sources.push_back(source);
				if (source.HasComponent<SceneFolderComponent>())
				{
					for (Entity child : level->GetFolderChildren(source))
						collect(child);
				}
				for (Entity child : level->GetChildren(source))
				{
					// Folder membership owns the outliner location when both relations exist.
					if (!level->GetEntityFolder(child))
						collect(child);
				}
			};
			collect(entity);

			std::unordered_map<uint32_t, Entity> duplicates;
			duplicates.reserve(sources.size());
			for (Entity source : sources)
			{
				Entity duplicate = level->DuplicateEntity(source, false);
				if (!duplicate)
					return {};
				duplicates.emplace(static_cast<uint32_t>(source), duplicate);
			}

			for (Entity source : sources)
			{
				Entity duplicate = duplicates.at(static_cast<uint32_t>(source));
				Entity sourceParent = level->GetParent(source);
				const auto duplicatedParent = sourceParent
					? duplicates.find(static_cast<uint32_t>(sourceParent))
					: duplicates.end();
				if (duplicatedParent != duplicates.end())
					level->SetEntityParent(duplicate, duplicatedParent->second, false);

				Entity sourceFolder = level->GetEntityFolder(source);
				const auto duplicatedFolder = sourceFolder
					? duplicates.find(static_cast<uint32_t>(sourceFolder))
					: duplicates.end();
				if (duplicatedFolder != duplicates.end())
					level->SetEntityFolder(duplicate, duplicatedFolder->second);
			}

			return duplicates.at(static_cast<uint32_t>(entity));
		}

		static bool TryReadEntityFromPayload(const ImGuiPayload* payload, UUID& outUuid)
		{
			if (!payload || !payload->Data)
				return false;

			try
			{
				outUuid = UUID(std::string((const char*)payload->Data));
				return outUuid.isValid();
			}
			catch (...)
			{
				return false;
			}
		}

		static void MarkLastItemEditedIfChanged(bool bChanged)
		{
			if (!bChanged)
				return;

			ImGuiID itemId = ImGui::GetItemID();
			if (itemId != 0)
				ImGui::MarkItemEdited(itemId);
		}
	}

	void SceneHierarchyPanel::SetContext(const Ref<Level>& context)
    {
        mContext = context;
        sScriptSelection.clear();
		if (mContext)
			mContext->MigrateLegacySceneFolderParents();
        sSelectionContext = {};
		sMeshSelectionContext = {};
		sRenameContext = {};
		memset(sRenameBuffer, 0, sizeof(sRenameBuffer));
		mHierarchyEntityCount = std::numeric_limits<size_t>::max();
		mHierarchySignature = std::numeric_limits<size_t>::max();
		mHierarchySignatureCountdown = 0;
		mPendingScrollEntity = {};
    }

	void SceneHierarchyPanel::OnImGuiRenderImpl()
    {
		HE_PROFILE;

		if (mContext)
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGuiWrapper::S(4.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImGuiWrapper::S(5.0f, 5.0f));
			if (ImGui::Button(EditorLayer::GetText("+ Add"), ImGuiWrapper::S(68.0f, 0.0f)))
				ImGui::OpenPopup("HierarchyAddMenu");
			if (ImGui::BeginPopup("HierarchyAddMenu"))
			{
				if (ImGui::MenuItem(EditorLayer::GetText("Empty Entity")))
				{
					if (mParent) mParent->QueueSceneOperations(Json::array({{{"op","create"},{"name",EditorLayer::GetText("Empty Entity")}}}));
				}
				if (ImGui::MenuItem(EditorLayer::GetText("Folder")))
					CreateFolder();
				ImGui::EndPopup();
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-1.0f);
			sHierarchyFilter.Draw("##HierarchySearch", -1.0f);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", EditorLayer::GetText("Search entities"));
			ImGui::Separator();
			ImGui::PopStyleVar(2);

			const size_t entityCount = mContext->mRegistry.view<IDComponent>().size();
			const bool bEntityCountChanged = entityCount != mHierarchyEntityCount;
			const bool bCheckHierarchySignature = bEntityCountChanged || mHierarchySignatureCountdown == 0;
			const size_t hierarchySignature = bCheckHierarchySignature
				? ComputeHierarchySignature()
				: mHierarchySignature;
			if (bCheckHierarchySignature)
				mHierarchySignatureCountdown = 15;
			else
				--mHierarchySignatureCountdown;
			if (bEntityCountChanged || hierarchySignature != mHierarchySignature)
			{
				RebuildHierarchyCache();
				mHierarchyEntityCount = entityCount;
				mHierarchySignature = hierarchySignature;
			}

			{
				HE_PROFILE_SECTION("Hierarchy Tree");
				DrawEntityList(mHierarchyRoots);
			}

			ImGuiWindow* window = ImGui::GetCurrentWindow();
			if (window)
			{
				const ImRect dropRect(window->WorkRect.Min, window->WorkRect.Max);
				if (ImGui::BeginDragDropTargetCustom(dropRect, ImGui::GetID("SceneHierarchyRootDropTarget")))
				{
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayloadType, ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
					{
						if (!ImGui::IsAnyItemHovered())
						{
							UUID payloadUuid;
							if (TryReadEntityFromPayload(payload, payloadUuid))
							{
								Entity draggedEntity = mContext->TryGetEntityByUUID(payloadUuid);
								if (draggedEntity && SetEditorHierarchyParent(draggedEntity, {}, mParent))
								{
									if (mParent)
										mParent->MarkSceneDirty("Reparent Entity");
								}
							}
						}
					}
					ImGui::EndDragDropTarget();
				}
			}

			if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered())
			{
				ClearSelection();
			}

			// Right-click on blank space
			if (ImGui::BeginPopupContextWindow(nullptr, ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
			{
				if (ImGui::BeginMenu(EditorLayer::GetText("Create")))
				{
					if (ImGui::MenuItem(EditorLayer::GetText("Folder")))
						CreateFolder();

					ImGui::Separator();

					if (ImGui::MenuItem(EditorLayer::GetText("Empty Entity")))
				{
					if (mParent) mParent->QueueSceneOperations(Json::array({{{"op","create"},{"name",EditorLayer::GetText("Empty Entity")}}}));
				}

					ImGui::Separator();

					if (ImGui::BeginMenu(EditorLayer::GetText("3D Object")))
					{
						if (ImGui::MenuItem(EditorLayer::GetText("Cube")))
						{
							if (mParent) mParent->QueueSceneOperations(Json::array({{{"op","create"},{"name",EditorLayer::GetText("Cube")},
                                {"components",{{"MeshComponent",{{"Path","Assets/Models/Cube/Cube.hmesh"}}}}}}}));
						}

						if (ImGui::MenuItem(EditorLayer::GetText("Sphere")))
						{
							auto entity = mContext->CreateEntity(EditorLayer::GetText("Sphere"));
							entity.AddComponent<MeshComponent>("Assets/Models/Sphere.obj");
							SetSelectedEntity(entity);
							if (mParent)
								mParent->MarkSceneDirty("Create Entity");
						}

						if (ImGui::MenuItem(EditorLayer::GetText("Plane")))
						{
							auto entity = mContext->CreateEntity(EditorLayer::GetText("Plane"));
							entity.AddComponent<MeshComponent>("Assets/Models/Box/Box.hmesh");
							auto& transform = entity.GetComponent<TransformComponent>();
							transform.Scale = glm::vec3(4.0f, 0.1f, 4.0f);
							SetSelectedEntity(entity);
							if (mParent)
								mParent->MarkSceneDirty("Create Entity");
						}

						ImGui::EndMenu();
					}

					if (ImGui::BeginMenu(EditorLayer::GetText("Light")))
					{
						if (ImGui::MenuItem(EditorLayer::GetText("Point Light")))
						{
							if (mParent) mParent->QueueSceneOperations(Json::array({{{"op","create"},{"name",EditorLayer::GetText("Point Light")},
                                {"components",{{"PointLightComponent",Json::object()}}}}}));
						}

						if (ImGui::MenuItem(EditorLayer::GetText("Directional Light")))
						{
							auto entity = mContext->CreateEntity(EditorLayer::GetText("Directional Light"));
							entity.AddComponent<DirectionalLightComponent>();
							entity.AddComponent<MeshComponent>("Assets/Models/Arrow.obj");
							SetSelectedEntity(entity);
							if (mParent)
								mParent->MarkSceneDirty("Create Entity");
						}

						if (ImGui::MenuItem(EditorLayer::GetText("Environment Light")))
						{
							auto entity = mContext->CreateEntity(EditorLayer::GetText("Environment Light"));
							entity.AddComponent<EnvironmentLightComponent>();
							SetSelectedEntity(entity);
							if (mParent)
								mParent->MarkSceneDirty("Create Entity");
						}

						ImGui::EndMenu();
					}

					if (ImGui::MenuItem(EditorLayer::GetText("Camera")))
					{
					auto entity = mContext->CreateEntity(EditorLayer::GetText("Camera"));
					entity.AddComponent<CameraComponent>();
					SetSelectedEntity(entity);
					if (mParent)
						mParent->MarkSceneDirty("Create Entity");
				}

					if (ImGui::MenuItem(EditorLayer::GetText("Audio Source")))
					{
					auto entity = mContext->CreateEntity(EditorLayer::GetText("Audio Source"));
					entity.AddComponent<SoundComponent>();
					SetSelectedEntity(entity);
					if (mParent)
						mParent->MarkSceneDirty("Create Entity");
				}

					if (ImGui::MenuItem(EditorLayer::GetText("Audio Listener")))
					{
						auto entity = mContext->CreateEntity(EditorLayer::GetText("Audio Listener"));
						entity.AddComponent<AudioListenerComponent>();
						SetSelectedEntity(entity);
						if (mParent)
							mParent->MarkSceneDirty("Create Entity");
					}

					ImGui::EndMenu();
				}

				ImGui::EndPopup();
			}
		}
    }

	Entity SceneHierarchyPanel::GetSelectedEntity() const
	{
		return sSelectionContext;
	}

	void SceneHierarchyPanel::SetSelectedEntity(Entity entity)
    {
		sScriptSelection.clear();
		if (entity) sScriptSelection.push_back(entity);
		sSelectionContext = entity;
		sMeshSelectionContext = {};
		mPendingScrollEntity = entity;
		for (Entity parent = GetEditorHierarchyParent(entity); parent; parent = GetEditorHierarchyParent(parent))
			SetEntityExpandedState(parent, true);
    }

	void SceneHierarchyPanel::ClearSelection()
	{
		sScriptSelection.clear();
		sSelectionContext = {};
		sMeshSelectionContext = {};
	}

	void SceneHierarchyPanel::SetSelectedEntity(Entity entity, const MeshSelectionContext& meshSelectionContext)
	{
		sScriptSelection.clear();
		if (entity) sScriptSelection.push_back(entity);
		sSelectionContext = entity;
		sMeshSelectionContext = meshSelectionContext;
		mPendingScrollEntity = entity;
		for (Entity parent = GetEditorHierarchyParent(entity); parent; parent = GetEditorHierarchyParent(parent))
			SetEntityExpandedState(parent, true);
	}

	Json SceneHierarchyPanel::ExportEditorState() const
	{
		Json state;
		state["SelectedEntity"] = sSelectionContext ? sSelectionContext.UUID().str() : std::string();
        state["SelectedEntities"] = Json::array();
        for (auto entity : sScriptSelection) if (entity) state["SelectedEntities"].push_back(entity.UUID().str());

		Json uiStates = Json::array();
		if (mContext)
		{
			const Level* level = mContext.get();
			auto levelIt = sEntityUiStates.find(level);
			if (levelIt != sEntityUiStates.end())
			{
				std::vector<std::pair<std::string, EntityUiState>> sortedStates;
				sortedStates.reserve(levelIt->second.size());
				for (const auto& [uuid, uiState] : levelIt->second)
					sortedStates.emplace_back(uuid.str(), uiState);

				std::sort(sortedStates.begin(), sortedStates.end(),
					[](const auto& lhs, const auto& rhs)
					{
						return lhs.first < rhs.first;
					});

				for (const auto& [uuid, uiState] : sortedStates)
				{
					Json item;
					item["Uuid"] = uuid;
					item["Visible"] = uiState.bVisible;
					item["Locked"] = uiState.bLocked;
					item["Expanded"] = uiState.bExpanded;
					uiStates.push_back(item);
				}
			}
		}
		state["EntityUiStates"] = uiStates;
		return state;
	}

	IEditorUndoClient* SceneHierarchyPanel::GetUndoClient()
	{
		return mParent ? mParent->GetSceneUndoClient() : nullptr;
	}

	void SceneHierarchyPanel::ImportEditorState(const Json& state)
	{
		if (!mContext)
			return;

		sEntityUiStates[mContext.get()].clear();
		if (state.contains("EntityUiStates") && state["EntityUiStates"].is_array())
		{
			for (const Json& item : state["EntityUiStates"])
			{
				const std::string uuidStr = item.value("Uuid", std::string());
				if (uuidStr.empty())
					continue;

				Entity entity = mContext->TryGetEntityByUUID(UUID(uuidStr));
				if (!entity)
					continue;

				EntityUiState& uiState = EnsureEntityUiState(entity);
				const bool bVisible = item.value("Visible", true);
				const bool bLocked = item.value("Locked", false);
				uiState.bVisible = bVisible;
				uiState.bLocked = bLocked;
				uiState.bExpanded = item.value("Expanded", true);
				if (!bVisible || bLocked)
				{
					auto& editorState = entity.AddOrReplaceComponent<EditorEntityStateComponent>(EditorEntityStateComponent{});
					editorState.bVisible = bVisible;
					editorState.bLocked = bLocked;
				}
			}
		}
		mContext->InvalidateEditorEntityStateCache();

		const std::string selectedUuid = state.value("SelectedEntity", std::string());
		if (!selectedUuid.empty())
		{
			Entity selected = mContext->TryGetEntityByUUID(UUID(selectedUuid));
			if (selected)
			{
				SetSelectedEntity(selected);
                for (const auto& id : state.value("SelectedEntities", Json::array()))
                {
                    auto other = mContext->TryGetEntityByUUID(UUID(id.get<std::string>()));
                    if (other && other != selected && CanSelectEntity(other)) sScriptSelection.push_back(other);
                }
				return;
			}
		}

		ClearSelection();
	}

	SceneHierarchyPanel::MeshSelectionContext SceneHierarchyPanel::GetMeshSelectionContext() const
	{
		return sMeshSelectionContext;
	}

	SceneHierarchyPanel::MeshSelectionContext SceneHierarchyPanel::ConsumeMeshSelectionContext()
	{
		MeshSelectionContext context = sMeshSelectionContext;
		sMeshSelectionContext = {};
		return context;
	}

	bool SceneHierarchyPanel::IsEntityVisible(Entity entity) const
	{
		return entity && entity.GetLevel() && entity.GetLevel()->IsEntityVisibleInEditor(entity);
	}

	Entity SceneHierarchyPanel::DuplicateEntityHierarchy(Entity entity)
	{
		if (mParent && entity && !entity.HasComponent<SceneFolderComponent>())
		{
			mParent->QueueSceneOperations(Json::array({{{"op","duplicate"},{"entity",entity.UUID().str()},{"children","subtree"}}}));
			return {};
		}
		return DuplicateEditorHierarchyEntity(mContext.get(), entity);
	}

	bool SceneHierarchyPanel::IsEntityLocked(Entity entity) const
	{
		return entity && entity.GetLevel() && entity.GetLevel()->IsEntityLockedInEditor(entity);
	}

	bool SceneHierarchyPanel::CanSelectEntity(Entity entity) const
	{
		return entity && IsEntityVisible(entity) && !IsEntityLocked(entity);
	}

	bool SceneHierarchyPanel::IsSelectedEntityLocked() const
	{
		return IsEntityLocked(sSelectionContext);
	}

	Entity SceneHierarchyPanel::CreateFolder(Entity parent)
	{
		if (!mContext)
			return {};

		Entity folder = mContext->CreateEntity(EditorLayer::GetText("New Folder"));
		folder.AddComponent<SceneFolderComponent>();
		folder.AddComponent<EditorEntityStateComponent>();
		if (parent)
			mContext->SetEntityFolder(folder, parent);
		SetEntityExpandedState(folder, true);
		SetSelectedEntity(folder);
		if (mParent)
			mParent->MarkSceneDirty("Create Folder");
		return folder;
	}

	bool SceneHierarchyPanel::MatchesFilter(Entity entity) const
	{
		if (!sHierarchyFilter.IsActive())
			return true;
		if (sHierarchyFilter.PassFilter(EntityDisplayName(entity).c_str()))
			return true;

		const auto childrenIt = mHierarchyChildren.find(static_cast<uint32_t>(entity));
		if (childrenIt == mHierarchyChildren.end())
			return false;
		for (Entity child : childrenIt->second)
		{
			if (MatchesFilter(child))
				return true;
		}
		return false;
	}

	size_t SceneHierarchyPanel::ComputeHierarchySignature() const
	{
		if (!mContext)
			return 0;

		size_t signature = mContext->mRegistry.view<IDComponent>().size();
		auto transformView = mContext->mRegistry.view<IDComponent, TransformComponent>();
		for (auto handle : transformView)
		{
			const auto& id = transformView.get<IDComponent>(handle).ID;
			const auto& transform = transformView.get<TransformComponent>(handle);
			const std::string folderUuid = mContext->mRegistry.all_of<EditorEntityStateComponent>(handle)
				? mContext->mRegistry.get<EditorEntityStateComponent>(handle).FolderUuid
				: std::string();
			const size_t entityHash = std::hash<std::string>{}(id.str());
			const size_t parentHash = std::hash<std::string>{}(transform.ParentUuid);
			const size_t folderHash = std::hash<std::string>{}(folderUuid);
			signature ^= entityHash + 0x9e3779b9u + (signature << 6u) + (signature >> 2u);
			signature ^= parentHash + 0x9e3779b9u + (signature << 6u) + (signature >> 2u);
			signature ^= folderHash + 0x9e3779b9u + (signature << 6u) + (signature >> 2u);
			if (mContext->mRegistry.all_of<TagComponent>(handle))
			{
				const size_t nameHash = std::hash<std::string>{}(mContext->mRegistry.get<TagComponent>(handle).Tag);
				signature ^= nameHash + 0x9e3779b9u + (signature << 6u) + (signature >> 2u);
			}
		}
		return signature;
	}

	void SceneHierarchyPanel::RebuildHierarchyCache()
	{
		HE_PROFILE;

		mHierarchyRoots.clear();
		mHierarchyChildren.clear();

		if (!mContext)
			return;

		auto sortEntities = [](std::vector<Entity>& entities)
			{
				std::stable_sort(entities.begin(), entities.end(), [](Entity lhs, Entity rhs)
					{
						const bool bLeftFolder = lhs.HasComponent<SceneFolderComponent>();
						const bool bRightFolder = rhs.HasComponent<SceneFolderComponent>();
						if (bLeftFolder != bRightFolder)
							return bLeftFolder;
						return EntityDisplayName(lhs) < EntityDisplayName(rhs);
					});
			};

		bool bHasParentedEntity = false;
		auto transformView = mContext->mRegistry.view<TransformComponent>();
		for (auto handle : transformView)
		{
			if (!transformView.get<TransformComponent>(handle).ParentUuid.empty())
			{
				bHasParentedEntity = true;
				break;
			}
		}
		if (!bHasParentedEntity)
		{
			auto editorStateView = mContext->mRegistry.view<EditorEntityStateComponent>();
			for (auto handle : editorStateView)
			{
				if (!editorStateView.get<EditorEntityStateComponent>(handle).FolderUuid.empty())
				{
					bHasParentedEntity = true;
					break;
				}
			}
		}

		if (!bHasParentedEntity)
		{
			mContext->mRegistry.each([this](auto entityID)
				{
					mHierarchyRoots.emplace_back(entityID, mContext.get());
				});
			sortEntities(mHierarchyRoots);
			return;
		}

		std::unordered_map<std::string, EntityHandle> entityByUuid;
		auto idView = mContext->mRegistry.view<IDComponent>();
		for (auto handle : idView)
		{
			entityByUuid.emplace(idView.get<IDComponent>(handle).ID.str(), handle);
		}

		mContext->mRegistry.each([this, &entityByUuid](auto entityID)
			{
				Entity entity = { entityID, mContext.get() };
				EntityHandle parentHandle = entt::null;

				if (mContext->mRegistry.all_of<EditorEntityStateComponent>(entityID))
				{
					const auto& editorState = mContext->mRegistry.get<EditorEntityStateComponent>(entityID);
					if (!editorState.FolderUuid.empty())
					{
						auto folderIt = entityByUuid.find(editorState.FolderUuid);
						if (folderIt != entityByUuid.end() && folderIt->second != entityID &&
							mContext->mRegistry.all_of<SceneFolderComponent>(folderIt->second))
						{
							parentHandle = folderIt->second;
						}
					}
				}

				if (parentHandle == entt::null && mContext->mRegistry.all_of<TransformComponent>(entityID))
				{
					const auto& transform = mContext->mRegistry.get<TransformComponent>(entityID);
					if (!transform.ParentUuid.empty())
					{
						auto parentIt = entityByUuid.find(transform.ParentUuid);
						if (parentIt != entityByUuid.end() && parentIt->second != entityID)
							parentHandle = parentIt->second;
					}
				}

				if (parentHandle == entt::null)
					mHierarchyRoots.push_back(entity);
				else
					mHierarchyChildren[(uint32_t)parentHandle].push_back(entity);
			});

		sortEntities(mHierarchyRoots);
		for (auto& [parent, children] : mHierarchyChildren)
		{
			(void)parent;
			sortEntities(children);
		}
	}

	void SceneHierarchyPanel::DrawEntityList(const std::vector<Entity>& entities)
	{
		if (sHierarchyFilter.IsActive() || mPendingScrollEntity)
		{
			for (Entity entity : entities)
				DrawEntityNode(entity);
			return;
		}

		size_t index = 0;
		while (index < entities.size())
		{
			const auto childrenIt = mHierarchyChildren.find(static_cast<uint32_t>(entities[index]));
			if (childrenIt != mHierarchyChildren.end() && !childrenIt->second.empty())
			{
				DrawEntityNode(entities[index++]);
				continue;
			}

			const size_t leafRunBegin = index;
			while (index < entities.size())
			{
				const auto leafChildrenIt = mHierarchyChildren.find(static_cast<uint32_t>(entities[index]));
				if (leafChildrenIt != mHierarchyChildren.end() && !leafChildrenIt->second.empty())
					break;
				++index;
			}

			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(index - leafRunBegin));
			while (clipper.Step())
				for (int visibleIndex = clipper.DisplayStart; visibleIndex < clipper.DisplayEnd; ++visibleIndex)
					DrawEntityNode(entities[leafRunBegin + static_cast<size_t>(visibleIndex)]);
		}
	}

	void SceneHierarchyPanel::DrawEntityNode(Entity entity)
	{
		if (!entity || !MatchesFilter(entity))
			return;

		const char* name = EditorLayer::GetText("Unnamed Entity");
		const bool bOwnVisible = GetEntityVisibleState(entity);
		const bool bOwnLocked = GetEntityLockedState(entity);
		const bool bVisible = IsEntityVisible(entity);
		const bool bLocked = IsEntityLocked(entity);
		const bool bFolder = entity.HasComponent<SceneFolderComponent>();
		const bool bHasStoredParent = static_cast<bool>(GetEditorHierarchyParent(entity));
		const auto childrenIt = mHierarchyChildren.find((uint32_t)entity);
		const bool bHasChildren = childrenIt != mHierarchyChildren.end() && !childrenIt->second.empty();
		if (entity.HasComponent<TagComponent>()) 
		{
			name = entity.GetComponent<TagComponent>().Tag.c_str();
		}

		ImGuiTreeNodeFlags flags = ((std::find(sScriptSelection.begin(), sScriptSelection.end(), entity) != sScriptSelection.end()) ? ImGuiTreeNodeFlags_Selected : 0) | ImGuiTreeNodeFlags_OpenOnArrow;
		flags |= ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap;
		if (!bHasChildren)
			flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		else
		{
			if (sHierarchyFilter.IsActive())
				ImGui::SetNextItemOpen(true, ImGuiCond_Always);
			else
			ImGui::SetNextItemOpen(GetEntityExpandedState(entity), ImGuiCond_Always);
		}

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImGuiWrapper::S(3.0f, 1.0f));
		bool opened = ImGui::TreeNodeEx((void*)(uint64_t)(uint32_t)entity, flags, "");
		ImGui::PopStyleVar();
		if (mPendingScrollEntity == entity)
		{
			ImGui::SetScrollHereY(0.5f);
			mPendingScrollEntity = {};
		}
		const ImVec2 nodeRectMin = ImGui::GetItemRectMin();
		const ImVec2 nodeRectMax = ImGui::GetItemRectMax();
		const ImRect rowRect(nodeRectMin, ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x, nodeRectMax.y));
		if (bHasChildren)
			SetEntityExpandedState(entity, opened);
		if (ImGui::IsItemClicked() && !bLocked)
		{
            if (ImGui::GetIO().KeyCtrl)
            {
                const auto selected = std::find(sScriptSelection.begin(), sScriptSelection.end(), entity);
                if (selected == sScriptSelection.end()) sScriptSelection.push_back(entity);
                else sScriptSelection.erase(selected);
                sSelectionContext = sScriptSelection.empty() ? Entity{} : sScriptSelection.back();
                sMeshSelectionContext = {};
            }
            else SetSelectedEntity(entity);
		}

		if (bLocked && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", EditorLayer::GetText("Locked Entity"));
		bool entityDeleted = false;
		bool entityDuplicate = false;
		bool bRequestRename = false;
		bool bDetachFromParent = false;
		if (ImGui::BeginPopupContextItem())
		{
            if (ImGui::MenuItem("Toggle Script Multi-Selection", nullptr, std::find(sScriptSelection.begin(), sScriptSelection.end(), entity) != sScriptSelection.end(), !bLocked))
            {
                const auto selected = std::find(sScriptSelection.begin(), sScriptSelection.end(), entity);
                if (selected == sScriptSelection.end()) sScriptSelection.push_back(entity);
                else sScriptSelection.erase(selected);
                sSelectionContext = sScriptSelection.empty() ? Entity{} : sScriptSelection.back();
                sMeshSelectionContext = {};
            }
			if (ImGui::MenuItem(EditorLayer::GetText("Duplicate Entity"), "Ctrl+D"))
				entityDuplicate = true;

			if (ImGui::MenuItem(EditorLayer::GetText("Rename"), "F2"))
			{
				bRequestRename = true;
				SetSelectedEntity(entity);
			}

			ImGui::Separator();
			if (bFolder && ImGui::MenuItem(EditorLayer::GetText("Create Folder")))
				CreateFolder(entity);
			if (bFolder)
				ImGui::Separator();

			if (bHasStoredParent && ImGui::MenuItem(EditorLayer::GetText("Detach From Parent")))
				bDetachFromParent = true;

			if (bHasStoredParent)
				ImGui::Separator();

			if (ImGui::MenuItem(EditorLayer::GetText("Save as Prefab")))
			{
				std::filesystem::path prefabDir = ConfigManager::GetFullPath("Assets/Prefabs");
				if (!std::filesystem::exists(prefabDir))
					std::filesystem::create_directories(prefabDir);

				std::filesystem::path savePath = FileDialogs::SaveFile(
					"HEngine Prefab (*.hprefab)\0*.hprefab\0\0",
					prefabDir.string());
				if (!savePath.empty())
				{
					if (savePath.extension() != ".hprefab")
						savePath += ".hprefab";

					PrefabSerializer::SavePrefab(entity, savePath.string());
				}
			}

			if (ImGui::MenuItem(EditorLayer::GetText(bFolder ? "Delete Folder" : "Delete Entity"), "Del"))
				entityDeleted = true;

			ImGui::EndPopup();
		}

		if (entity.HasComponent<IDComponent>() && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
		{
			const std::string uuidStr = entity.UUID().str();
			ImGui::SetDragDropPayload(kEntityPayloadType, uuidStr.c_str(), uuidStr.size() + 1);
			ImGui::Text("%s", name);
			ImGui::EndDragDropSource();
		}

		if (bRequestRename)
		{
			sRenameContext = entity;
			memset(sRenameBuffer, 0, sizeof(sRenameBuffer));
			std::snprintf(sRenameBuffer, sizeof(sRenameBuffer), "%s", name);
		}

		const float rowHeight = ImGui::GetItemRectSize().y;
		const float iconSize = ImMax(ImGuiWrapper::S(13.0f), rowHeight - ImGuiWrapper::S(2.0f));
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor(1.0f, 1.0f, 1.0f, 0.0f));
		const std::string entityIconName = PickEntityIconName(entity);
		const ImGuiTextureHandle entityIcon = GetIconOrFallback(entityIconName, "EntityIcon");
		if (entityIcon)
			ImGui::Image(IconManager::ToImTextureID(entityIcon), ImVec2{ iconSize, iconSize });
		ImGui::PopStyleColor(1);
		ImGui::SameLine();

		if (sRenameContext == entity)
		{
			ImGui::SetNextItemWidth(ImGuiWrapper::S(180.0f));
			if (ImGui::InputText((std::string("##Rename") + std::to_string((uint32_t)entity)).c_str(), sRenameBuffer, sizeof(sRenameBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
			{
				if (entity.HasComponent<TagComponent>() && strlen(sRenameBuffer) > 0)
					if (mParent) mParent->QueueSceneOperations(Json::array({{{"op","rename"},{"entity",entity.UUID().str()},{"name",sRenameBuffer}}}));
				if (mParent)
					mParent->MarkSceneDirty("Rename Entity");
				sRenameContext = {};
			}
			if (ImGui::IsItemDeactivatedAfterEdit())
			{
				if (entity.HasComponent<TagComponent>() && strlen(sRenameBuffer) > 0)
					if (mParent) mParent->QueueSceneOperations(Json::array({{{"op","rename"},{"entity",entity.UUID().str()},{"name",sRenameBuffer}}}));
				if (mParent)
					mParent->MarkSceneDirty("Rename Entity");
				sRenameContext = {};
			}
		}
		else
		{
			if (bVisible)
				ImGui::Text(name);
			else
				ImGui::TextDisabled("%s", name);
		}

		const float buttonSize = ImGuiWrapper::S(14.0f);
		const float rightPadding = ImGui::GetStyle().ScrollbarSize + ImGuiWrapper::S(8.0f);
		const float buttonSpacing = ImGuiWrapper::S(4.0f);
		const float rightStartX = ImGui::GetWindowContentRegionMax().x - (buttonSize * 2.0f + buttonSpacing + rightPadding);
		ImGui::SameLine(rightStartX);
		ImGui::PushID(static_cast<int>(static_cast<uint32_t>(entity)));

		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.14f));

		const ImGuiTextureHandle visibleIcon = GetIconOrFallback(bVisible ? "VisibleIcon" : "HiddenIcon", "EntityIcon");
		if (visibleIcon)
		{
			ImVec4 visTint = bVisible ? ImVec4(0.78f, 0.78f, 0.82f, 1.00f) : ImVec4(0.42f, 0.42f, 0.46f, 0.85f);
			if (ImGui::ImageButton("##Visibility", IconManager::ToImTextureID(visibleIcon), ImVec2{ buttonSize, buttonSize }, ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), visTint))
			{
				SetEntityVisibleState(entity, !bOwnVisible);
				if (mParent)
					mParent->MarkSceneDirty("Toggle Entity Visibility");
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", EditorLayer::GetText(bVisible ? "Hide Entity" : "Show Entity"));
		}

		ImGui::SameLine(0.0f, buttonSpacing);
		const ImGuiTextureHandle lockIcon = GetIconOrFallback(bLocked ? "LockedIcon" : "UnlockedIcon", "SettingIcon");
		if (lockIcon)
		{
			ImVec4 lockTint = bLocked ? ImVec4(1.0f, 0.55f, 0.30f, 1.0f) : ImVec4(0.58f, 0.58f, 0.62f, 0.90f);
			if (ImGui::ImageButton("##Lock", IconManager::ToImTextureID(lockIcon), ImVec2{ buttonSize, buttonSize }, ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), lockTint))
			{
				SetEntityLockedState(entity, !bOwnLocked);
				if (mParent)
					mParent->MarkSceneDirty("Toggle Entity Lock");
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", EditorLayer::GetText(bLocked ? "Unlock Entity" : "Lock Entity"));
		}

		ImGui::PopStyleColor(3);
		ImGui::PopID();

		if (ImGui::BeginDragDropTargetCustom(rowRect, ImGui::GetID((std::string("HierarchyReparentTarget") + std::to_string((uint32_t)entity)).c_str())))
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayloadType, ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
			{
				UUID payloadUuid;
				if (TryReadEntityFromPayload(payload, payloadUuid))
				{
					Entity draggedEntity = mContext->TryGetEntityByUUID(payloadUuid);
					if (draggedEntity && draggedEntity != entity &&
						!IsEditorHierarchyDescendantOf(entity, draggedEntity) && SetEditorHierarchyParent(draggedEntity, entity, mParent))
					{
						SetEntityExpandedState(entity, true);
						if (mParent)
							mParent->MarkSceneDirty("Reparent Entity");
					}
				}
			}
			ImGui::EndDragDropTarget();
		}

		if (opened && bHasChildren)
		{
			DrawEntityList(childrenIt->second);
			ImGui::TreePop();
		}

		if (bDetachFromParent)
		{
			if (SetEditorHierarchyParent(entity, {}, mParent) && mParent)
				mParent->MarkSceneDirty("Reparent Entity");
		}

		if (entityDeleted)
		{
			const bool bDeletedSelectedEntity = sSelectionContext && (sSelectionContext == entity || IsEditorHierarchyDescendantOf(sSelectionContext, entity));
			if (sRenameContext && (sRenameContext == entity || IsEditorHierarchyDescendantOf(sRenameContext, entity)))
				sRenameContext = {};
			if (bFolder)
			{
				Entity folderParent = mContext->GetEntityFolder(entity);
				for (Entity child : mContext->GetFolderChildren(entity))
					mContext->SetEntityFolder(child, folderParent);
			}
			RemoveEntityUiStateRecursive(mContext, entity);
			if (!bFolder && mParent)
				mParent->QueueSceneOperations(Json::array({{{"op","delete"},{"entity",entity.UUID().str()},{"children","subtree"}}}));
			else mContext->DestroyEntity(entity);
			if (bDeletedSelectedEntity)
				ClearSelection();
			if (mParent)
				mParent->MarkSceneDirty("Delete Entity");
		}

		if (entityDuplicate)
		{
			Entity duplicatedEntity = DuplicateEntityHierarchy(entity);
			if (duplicatedEntity)
			{
				SetSelectedEntity(duplicatedEntity);
				Entity duplicatedParent = GetEditorHierarchyParent(duplicatedEntity);
				if (duplicatedParent)
					SetEntityExpandedState(duplicatedParent, true);
			}
			if (mParent)
				mParent->MarkSceneDirty("Duplicate Entity");
		}
	}

	static void DrawVec3Control(const std::string& label, glm::vec3& values, float resetValue = 0.0f, float columnWidth = ImGuiWrapper::kDefaultPropertyLabelWidth)
    {
        ImGui::PushID(label.c_str());

		DrawLabeledControl(label.c_str(), [&]()
		{
			const float contentWidth = ImGui::GetContentRegionAvail().x;
			const float compactSpacing = ImGuiWrapper::S(2.0f);
			const float frameHeight = ImGui::GetFrameHeight();
			const float axisButtonWidth = ImGuiWrapper::S(14.0f);
			const float axisBlockWidth = (contentWidth - compactSpacing * 2.0f) / 3.0f;
			const float dragWidth = ImMax(ImGuiWrapper::S(24.0f), axisBlockWidth - axisButtonWidth - compactSpacing);

			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImGuiWrapper::S(2.0f, 2.0f));

		auto drawAxis = [&](const char* axisLabel, float& value, const ImVec4& btnColor, const ImVec4& hoverColor, const ImVec4& activeColor)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, btnColor);
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverColor);
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeColor);
				if (ImGui::Button(axisLabel, ImVec2(axisButtonWidth, frameHeight)))
				{
					value = resetValue;
					MarkLastItemEditedIfChanged(true);
				}
				ImGui::PopStyleColor(3);

				ImGui::SameLine(0.0f, compactSpacing);
				ImGui::SetNextItemWidth(dragWidth);
				ImGui::DragFloat((std::string("##") + axisLabel).c_str(), &value, 0.1f, 0.0f, 0.0f, "%.2f");
			};

			drawAxis("X", values.x, ImVec4(0.80f, 0.18f, 0.20f, 1.0f), ImVec4(0.93f, 0.28f, 0.30f, 1.0f), ImVec4(0.72f, 0.14f, 0.16f, 1.0f));
			ImGui::SameLine(0.0f, compactSpacing);
			drawAxis("Y", values.y, ImVec4(0.24f, 0.72f, 0.28f, 1.0f), ImVec4(0.33f, 0.82f, 0.36f, 1.0f), ImVec4(0.20f, 0.62f, 0.24f, 1.0f));
			ImGui::SameLine(0.0f, compactSpacing);
			drawAxis("Z", values.z, ImVec4(0.24f, 0.46f, 0.85f, 1.0f), ImVec4(0.33f, 0.56f, 0.95f, 1.0f), ImVec4(0.18f, 0.38f, 0.75f, 1.0f));

			ImGui::PopStyleVar();
		}, columnWidth);

        ImGui::PopID();
    }

	template <typename T>
	static ImVec4 GetComponentCategoryColor()
	{
		if constexpr (std::is_same_v<T, TransformComponent>)
			return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
		else if constexpr (std::is_same_v<T, CameraComponent> || std::is_same_v<T, FollowCameraComponent> || std::is_same_v<T, SpringArmComponent>)
			return ImVec4(0.60f, 0.40f, 0.85f, 1.0f);
		else if constexpr (std::is_same_v<T, MeshComponent> || std::is_same_v<T, SpriteRendererComponent> || std::is_same_v<T, CircleRendererComponent>)
			return ImVec4(0.30f, 0.55f, 0.85f, 1.0f);
		else if constexpr (std::is_same_v<T, PointLightComponent> || std::is_same_v<T, DirectionalLightComponent> || std::is_same_v<T, EnvironmentLightComponent>)
			return ImVec4(0.90f, 0.80f, 0.30f, 1.0f);
		else if constexpr (std::is_same_v<T, PostProcessingComponent>)
			return ImVec4(0.55f, 0.42f, 0.86f, 1.0f);
		else if constexpr (std::is_same_v<T, Rigidbody3DComponent> || std::is_same_v<T, Rigidbody2DComponent> || std::is_same_v<T, CapsuleCollider3DComponent> || std::is_same_v<T, BoxCollider2DComponent> || std::is_same_v<T, CircleCollider2DComponent>)
			return ImVec4(0.30f, 0.75f, 0.40f, 1.0f);
		else if constexpr (std::is_same_v<T, AnimatorComponent> || std::is_same_v<T, CharacterMovementComponent>)
			return ImVec4(0.90f, 0.55f, 0.20f, 1.0f);
		else if constexpr (std::is_same_v<T, PythonScriptComponent>)
			return ImVec4(0.85f, 0.35f, 0.35f, 1.0f);
		else if constexpr (std::is_same_v<T, SoundComponent> || std::is_same_v<T, AudioListenerComponent>)
			return ImVec4(0.30f, 0.80f, 0.75f, 1.0f);
		else
			return ImVec4(0.72f, 0.72f, 0.72f, 1.0f);
	}

	template <typename T, typename UIFunction>
	static void DrawComponent(const std::string& name, Entity entity, UIFunction uiFunction, const std::string& iconName = "")
	{
		(void)iconName;

		const ImGuiTreeNodeFlags treeNodeFlags = ImGuiTreeNodeFlags_DefaultOpen |
			ImGuiTreeNodeFlags_Framed |
			ImGuiTreeNodeFlags_SpanAvailWidth |
			ImGuiTreeNodeFlags_AllowOverlap |
			ImGuiTreeNodeFlags_FramePadding;

		if (!entity.HasComponent<T>())
			return;

		auto& component = entity.GetComponent<T>();

		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGuiWrapper::S(4.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImGuiWrapper::S(5.0f, 3.0f));
		ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.18f, 0.18f, 0.21f, 0.70f));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.30f, 0.55f, 0.85f, 0.50f));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.30f, 0.55f, 0.85f, 0.65f));

		const float headerHeight = ImGui::GetFrameHeight();
		bool open = ImGuiWrapper::TreeNodeExStyle1((void*)typeid(T).hash_code(), EditorLayer::GetText(name.c_str()), treeNodeFlags);

		const ImVec2 nodeRectMin = ImGui::GetItemRectMin();
		const ImVec2 nodeRectMax = ImGui::GetItemRectMax();
		const float settingsButtonSize = ImGuiWrapper::S(14.0f);
		const float settingsX = nodeRectMax.x - settingsButtonSize - ImGuiWrapper::S(6.0f);
		const float settingsY = nodeRectMin.y + (headerHeight - settingsButtonSize) * 0.5f;
		ImGui::SetCursorScreenPos(ImVec2(settingsX, settingsY));

		if (ImGui::ImageButton("##ComponentSettings", IconManager::ToImTextureID(IconManager::GetInstance().GetSettingIcon()), ImVec2(settingsButtonSize, settingsButtonSize), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), ImVec4(0.90f, 0.90f, 0.92f, 0.90f)))
		{
			ImGui::OpenPopup("ComponentSettings");
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", EditorLayer::GetText("Component Settings"));
		}

		bool removeComponent = false;
		if (ImGui::BeginPopup("ComponentSettings"))
		{
            if constexpr (std::is_same_v<T, ScriptComponent>)
            {
                static std::optional<ScriptComponent> clipboard;
                if (ImGui::MenuItem("Copy Script")) clipboard = component;
                if (ImGui::MenuItem("Paste Script", nullptr, false, clipboard.has_value()))
                {
                    try
                    {
                        auto pasted = *clipboard;
                        if (const auto backend = ScriptBackendRegistry::Find(pasted.Backend))
                            if (auto* validator = dynamic_cast<IScriptEditorFields*>(backend.get()))
                                pasted.Fields = EncodeScriptFields(validator->EditFields(pasted.Type, DecodeScriptFields(pasted.Fields), false));
                        Json patch;
                        Serializer::Serialize(patch, pasted);
                        if (auto* editor = EditorLayer::GetPanel<PropertiesPanel>()->GetParent())
                            editor->QueueSceneOperations(Json::array({{{"op", "patch"}, {"entity", entity.UUID().str()}, {"component", "ScriptComponent"}, {"patch", patch}}}));
                    }
                    catch (const std::exception& error) { HE_CORE_ERROR("Paste Script rejected; original fields retained: {0}", error.what()); }
                }
            }
			if (ImGui::MenuItem(EditorLayer::GetText("Remove component")))
				removeComponent = true;

			ImGui::EndPopup();
		}

		if (open)
		{
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const ImVec4 categoryColor = GetComponentCategoryColor<T>();
			const float lineWidth = ImGuiWrapper::S(3.0f);
			const float contentIndent = lineWidth + ImGuiWrapper::S(6.0f);

			ImVec2 contentStartPos = ImGui::GetCursorScreenPos();
			ImGui::Indent(contentIndent);
			uiFunction(component);
			ImGui::Unindent(contentIndent);
			ImVec2 contentEndPos = ImGui::GetCursorScreenPos();

			const float lineTop = contentStartPos.y;
			const float lineBottom = ImMax(contentEndPos.y, lineTop + ImGuiWrapper::S(18.0f));
			drawList->AddRectFilled(
				ImVec2(contentStartPos.x, lineTop),
				ImVec2(contentStartPos.x + lineWidth, lineBottom),
				ImGui::GetColorU32(categoryColor));

			ImGui::TreePop();
		}

		if (removeComponent)
		{
			EditorLayer* editorLayer = EditorLayer::GetPanel<PropertiesPanel>()->GetParent();
			if constexpr (std::is_same_v<T,CameraComponent> || std::is_same_v<T,PointLightComponent> ||
				std::is_same_v<T,DirectionalLightComponent> || std::is_same_v<T,MeshComponent> || std::is_same_v<T,TransformComponent> || std::is_same_v<T,ScriptComponent>)
			{
				if (editorLayer) editorLayer->QueueSceneOperations(Json::array({{{"op","remove"},{"entity",entity.UUID().str()},{"component",T::GetTypeName()}}}));
			}
			else
			{
				entity.RemoveComponent<T>();
				if (editorLayer) editorLayer->MarkSceneDirty("Remove Component");
			}
		}

		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar(2);
		ImGui::Dummy(ImGuiWrapper::S(0.0f, 1.0f));
	}

	template <typename componentType>
	void PropertiesPanel::MenuAddComponent(const char* menuName, const char* menuItemName)
	{
		if (!sSelectionContext.HasComponent<componentType>())
		{
			if (ImGui::BeginMenu(EditorLayer::GetText(menuName)))
			{
				if (ImGui::MenuItem(EditorLayer::GetText(menuItemName), nullptr, false, IsBuildFeatureEnabled(componentType::RequiredBuildFeature)))
				{
					if constexpr (std::is_same_v<componentType,CameraComponent> || std::is_same_v<componentType,PointLightComponent> || std::is_same_v<componentType,DirectionalLightComponent>)
					{
						if (mParent) mParent->QueueSceneOperations(Json::array({{{"op","add"},{"entity",sSelectionContext.UUID().str()},
							{"component",componentType::GetTypeName()},{"patch",Json::object()}}}));
					}
					else
					{
						sSelectionContext.AddComponent<componentType>();
						if (mParent) mParent->MarkSceneDirty("Add Component");
					}
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndMenu();
			}
		}
	}

    void PropertiesPanel::DrawPrefabInstance(Entity entity)
    {
        if (!entity.HasComponent<PrefabInstanceComponent>() || !mParent) return;
        if (!ImGui::CollapsingHeader("Prefab Instance", ImGuiTreeNodeFlags_DefaultOpen)) return;
        const auto& prefab = entity.GetComponent<PrefabInstanceComponent>();
        ImGui::TextWrapped("Template: %s", prefab.TemplatePath.c_str());
        ImGui::TextWrapped("Local ID: %s", prefab.LocalId.c_str());
        ImGui::TextWrapped("Revision: %s", prefab.TemplateRevision.c_str());
        if (!mParent->GetLastSceneEditError().empty())
            ImGui::TextWrapped("Operation failed: %s", mParent->GetLastSceneEditError().c_str());
        const Json paths = Json::parse(prefab.OverridePathsJson, nullptr, false);
        if (!paths.is_array()) { ImGui::TextWrapped("Invalid override metadata; repair the scene before editing."); return; }
        ImGui::BeginDisabled(!ModeManager::IsEditState());
        if (ImGui::Button("Sync instance"))
            mParent->QueueSceneOperations(Json::array({{{"op", "prefab_sync"}, {"entity", prefab.InstanceRootUuid}}}));
        ImGui::SameLine();
        if (ImGui::Button("Unpack instance"))
            mParent->QueueSceneOperations(Json::array({{{"op", "prefab_unpack"}, {"entity", prefab.InstanceRootUuid}}}));
        ImGui::TextWrapped("Sync keeps overrides. Unpack removes the template link. Both support Undo.");
        ImGui::Text("Overrides on this entity: %zu", paths.size());
        const std::string id = entity.UUID().str();
        const auto clear = [&](const std::string& pointer) {
            return Json{{"op", "prefab_override"}, {"entity", id}, {"pointer", pointer}, {"mode", "clear"}};
        };
        if (!paths.empty() && ImGui::Button("Revert all entity overrides"))
        {
            Json operations = Json::array();
            for (const auto& path : paths)
            {
                if (!path.is_string()) continue;
                const std::string pointer = path.get<std::string>();
                bool covered = false;
                for (const auto& other : paths)
                    if (other.is_string() && pointer.starts_with(other.get<std::string>() + "/")) covered = true;
                if (!covered) operations.push_back(clear(pointer));
            }
            if (!operations.empty()) mParent->QueueSceneOperations(std::move(operations));
        }
        const Json baseline = Json::parse(prefab.BaselineJson, nullptr, false);
        for (const auto& path : paths)
        {
            if (!path.is_string()) continue;
            const std::string pointer = path.get<std::string>();
            ImGui::PushID(pointer.c_str());
            if (ImGui::SmallButton("Revert")) mParent->QueueSceneOperations(Json::array({clear(pointer)}));
            ImGui::SameLine();
            ImGui::TextWrapped("%s", pointer.c_str());
            if (ImGui::IsItemHovered() && baseline.is_object())
            {
                try
                {
                    const Json::json_pointer field(pointer);
                    const std::string value = baseline.contains(field) ? baseline.at(field).dump().substr(0, 2048) : "<absent in template>";
                    ImGui::SetTooltip("Template value: %s", value.c_str());
                }
                catch (const Json::exception&) { ImGui::SetTooltip("Invalid override path"); }
            }
            ImGui::PopID();
        }
        ImGui::EndDisabled();
    }

    void PropertiesPanel::DrawComponents(Entity entity)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGuiWrapper::S(3.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImGuiWrapper::S(4.0f, 2.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImGuiWrapper::S(5.0f, 3.0f));
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.11f, 0.14f, 0.18f, 0.80f));
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.17f, 0.21f, 0.28f, 0.95f));
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.24f, 0.33f, 0.85f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.23f, 0.31f, 0.44f, 0.95f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.28f, 0.40f, 1.0f));

		const bool bFolder = entity.HasComponent<SceneFolderComponent>();
		if (entity.HasComponent<TagComponent>())
		{
			auto& tag = entity.GetComponent<TagComponent>().Tag;

			char buffer[256];
			memset(buffer, 0, sizeof(buffer));
			std::snprintf(buffer, sizeof(buffer), "%s", tag.c_str());
			const float addButtonWidth = bFolder ? 0.0f : ImGuiWrapper::S(116.0f);
			ImGui::SetNextItemWidth(bFolder ? -1.0f : ImMax(ImGuiWrapper::S(80.0f), ImGui::GetContentRegionAvail().x - addButtonWidth - ImGuiWrapper::S(6.0f)));
			if (ImGui::InputText("##Tag", buffer, sizeof(buffer)))
			{
				tag = std::string(buffer);
			}
		}

		if (bFolder)
		{
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1.0f), "%s", EditorLayer::GetText("Scene Folder"));
			const size_t childCount = entity.GetLevel() ? entity.GetLevel()->GetFolderChildren(entity).size() : 0;
			ImGui::TextDisabled(EditorLayer::GetText("Contains %zu direct entities"), childCount);
			ImGui::TextWrapped("%s", EditorLayer::GetText("Drag entities onto this folder in Scene Hierarchy to organize the scene."));
			ImGui::PopStyleColor(5);
			ImGui::PopStyleVar(3);
			return;
		}

        ImGui::SameLine();
        ImGui::PushItemWidth(-1);

		if (ImGui::Button(EditorLayer::GetText("Add Component")))
            ImGui::OpenPopup("AddComponent");

        if (ImGui::BeginPopup("AddComponent"))
        {
			MenuAddComponent<CameraComponent>("Camera", "Camera");

			if (ModeManager::b3DMode)
			{
				MenuAddComponent<MeshComponent>("Mesh", "Mesh Renderer");
				MenuAddComponent<Rigidbody3DComponent>("Physic", "Rigidbody 3D");
				MenuAddComponent<CapsuleCollider3DComponent>("Physic", "Capsule Collider 3D");

				if (sSelectionContext.HasComponent<CameraComponent>())
				{
					MenuAddComponent<FollowCameraComponent>("Camera", "Follow Camera");
					MenuAddComponent<SpringArmComponent>("Camera", "Spring Arm");
				}

				MenuAddComponent<CharacterMovementComponent>("Character", "Character Movement");

				if (sSelectionContext.HasComponent<MeshComponent>())
					MenuAddComponent<AnimatorComponent>("Animation", "Animator");
					MenuAddComponent<SequenceComponent>("Animation", "Sequence");

				MenuAddComponent<PointLightComponent>("Light", "Point Light");
				MenuAddComponent<SpotLightComponent>("Light", "Spot Light");
				MenuAddComponent<DirectionalLightComponent>("Light", "Directional Light");
				MenuAddComponent<EnvironmentLightComponent>("Light", "Environment Light");
				MenuAddComponent<ReflectionProbeComponent>("Light", "Reflection Probe");
				MenuAddComponent<BakedIrradianceVolumeComponent>("Light", "Baked Irradiance Volume");
				MenuAddComponent<PostProcessingComponent>("Rendering", "Post Processing");
			}
			else
			{
				MenuAddComponent<SpriteRendererComponent>("Renderer", "Sprite Renderer");
				MenuAddComponent<CircleRendererComponent>("Renderer", "Circle Renderer");
				if (sSelectionContext.HasComponent<SpriteRendererComponent>() || sSelectionContext.HasComponent<CircleRendererComponent>())
				{
					MenuAddComponent<Rigidbody2DComponent>("Physic", "Rigidbody");
					MenuAddComponent<BoxCollider2DComponent>("Physic", "Box Collider");
					MenuAddComponent<CircleCollider2DComponent>("Physic", "Circle Collider");
				}
			}

			MenuAddComponent<PythonScriptComponent>("Script", "Python Script");
			MenuAddComponent<ScriptComponent>("Script", "Script");
			MenuAddComponent<SoundComponent>("Audio", "Sound");
			MenuAddComponent<AudioListenerComponent>("Audio", "Listener");

            ImGui::EndPopup();
        }

        ImGui::PopItemWidth();

		bool bEntityVisible = GetEntityVisibleState(entity);
		if (ImGui::Checkbox(EditorLayer::GetText("Visible"), &bEntityVisible))
		{
			SetEntityVisibleState(entity, bEntityVisible);
			if (mParent)
				mParent->MarkSceneDirty("Toggle Entity Visibility");
		}
		ImGui::SameLine(0.0f, ImGuiWrapper::S(14.0f));
		bool bEntityLocked = GetEntityLockedState(entity);
		if (ImGui::Checkbox(EditorLayer::GetText("Locked"), &bEntityLocked))
		{
			SetEntityLockedState(entity, bEntityLocked);
			if (mParent)
				mParent->MarkSceneDirty("Toggle Entity Lock");
		}
		ImGui::Separator();

        DrawPrefabInstance(entity);
		DrawComponent<SequenceComponent>("Sequence", entity, [this, entity](SequenceComponent& component)
		{
			if (DrawSequenceInspector(entity, component) && mParent)
				mParent->MarkSceneDirty("Edit Sequence");
		});

		DrawComponent<TransformComponent>("Transform", entity, [this, entity](auto& component)
			{
				Level* level = entity.GetLevel();
				Entity parentEntity = level ? level->GetParent(entity) : Entity{};
				const bool bHasStoredParent = !component.ParentUuid.empty();
				const std::string parentLabel = parentEntity
					? EntityDisplayName(parentEntity)
					: (bHasStoredParent ? std::string("Missing Parent") : std::string("None"));

				DrawLabeledControl("Parent", [&]()
					{
						ImGui::Button(parentLabel.c_str(), ImVec2(-1.0f, 0.0f));
						if (ImGui::BeginDragDropTarget())
						{
							if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayloadType))
							{
								UUID payloadUuid;
								if (level && TryReadEntityFromPayload(payload, payloadUuid))
								{
									Entity draggedEntity = level->TryGetEntityByUUID(payloadUuid);
									if (draggedEntity && draggedEntity != entity &&
										!draggedEntity.HasComponent<SceneFolderComponent>() && level->SetEntityParent(entity, draggedEntity, true))
									{
										MarkLastItemEditedIfChanged(true);
										if (mParent)
											mParent->MarkSceneDirty("Reparent Entity");
									}
								}
							}
							ImGui::EndDragDropTarget();
						}
					});

				DrawLabeledControl("Attachment", [&]()
					{
						if (!parentEntity)
							ImGui::BeginDisabled();
						if (ImGui::Button(EditorLayer::GetText("Select Parent")) && parentEntity)
							EditorLayer::GetPanel<SceneHierarchyPanel>()->SetSelectedEntity(parentEntity);
						if (!parentEntity)
							ImGui::EndDisabled();

						ImGui::SameLine();
						if (!bHasStoredParent)
							ImGui::BeginDisabled();
						if (ImGui::Button(EditorLayer::GetText("Clear")) && level && level->SetEntityParent(entity, {}, true))
						{
							MarkLastItemEditedIfChanged(true);
							if (mParent)
								mParent->MarkSceneDirty("Reparent Entity");
						}
						if (!bHasStoredParent)
							ImGui::EndDisabled();
					});

				glm::vec3 translation = component.Translation, scale = component.Scale;
				glm::vec3 rotation = glm::degrees(component.Rotation);
				DrawVec3Control("Location", translation);
				DrawVec3Control("Rotation", rotation);
				DrawVec3Control("Scale", scale, 1.0f);
				if (mParent && (translation != component.Translation || scale != component.Scale || rotation != glm::degrees(component.Rotation)))
					mParent->QueueSceneOperations(Json::array({{{"op","transform"},{"entity",Entity(entity).UUID().str()},
						{"patch",{{"Translation",translation},{"Rotation",glm::radians(rotation)},{"Scale",scale}}}}}));
			}, "TransformCompIcon");

		DrawComponent<CameraComponent>("Camera", entity, [](auto& component)
			{
				auto& camera = component.Camera;

				DrawLabeledControl("Primary", [&component]() { ImGui::Checkbox("##Primary", &component.Primary); });
				DrawLabeledControl("Override Exposure", [&component]()
					{
						ImGui::Checkbox("##CameraOverrideExposure", &component.bOverrideExposure);
					});
				if (!component.bOverrideExposure)
					ImGui::BeginDisabled();
				DrawLabeledControl("Exposure", [&component]()
					{
						ImGui::DragFloat("##CameraExposure", &component.ExposureEV100, 0.05f, -24.0f, 32.0f, "%.2f EV100", ImGuiSliderFlags_AlwaysClamp);
					});
				DrawLabeledControl("Exposure Compensation", [&component]() { ImGui::DragFloat("##CameraExposureCompensation", &component.ExposureCompensation, 0.05f, -24.0f, 24.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp); }, ImGuiWrapper::kWidePropertyLabelWidth);
				if (!component.bOverrideExposure)
					ImGui::EndDisabled();

				const char* projectionTypeStrings[] = { "Perspective", "Orthographic" };
				const char* currentProjectionTypeString = projectionTypeStrings[(int)camera.GetProjectionType()];
				DrawLabeledControl("Projection", [&]()
				{
					if (ImGui::BeginCombo("##Projection", EditorLayer::GetText(currentProjectionTypeString)))
					{
						for (int i = 0; i < 2; i++)
						{
							bool isSelected = (camera.GetProjectionType() == (SceneCamera::ProjectionType)i);
							if (ImGui::Selectable(EditorLayer::GetText(projectionTypeStrings[i]), isSelected))
							{
								currentProjectionTypeString = projectionTypeStrings[i];
								camera.SetProjectionType((SceneCamera::ProjectionType)i);
							}

							if (isSelected)
								ImGui::SetItemDefaultFocus();
						}

						ImGui::EndCombo();
					}
				});

				if (camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective)
				{
					float perspectiveVerticalFov = glm::degrees(camera.GetPerspectiveVerticalFOV());
					bool bVerticalFovChanged = false;
					DrawLabeledControl("Vertical FOV", [&]() { bVerticalFovChanged = ImGui::DragFloat("##PerspectiveVerticalFOV", &perspectiveVerticalFov, 0.1f, 1.0f, 179.0f); });
					if (bVerticalFovChanged)
						camera.SetPerspectiveVerticalFOV(glm::radians(perspectiveVerticalFov));

					float perspectiveNear = camera.GetPerspectiveNearClip();
					bool bPerspectiveNearChanged = false;
					DrawLabeledControl("Near", [&]() { bPerspectiveNearChanged = ImGui::DragFloat("##PerspectiveNear", &perspectiveNear); });
					if (bPerspectiveNearChanged)
						camera.SetPerspectiveNearClip(perspectiveNear);

					float perspectiveFar = camera.GetPerspectiveFarClip();
					bool bPerspectiveFarChanged = false;
					DrawLabeledControl("Far", [&]() { bPerspectiveFarChanged = ImGui::DragFloat("##PerspectiveFar", &perspectiveFar); });
					if (bPerspectiveFarChanged)
						camera.SetPerspectiveFarClip(perspectiveFar);
				}

				if (camera.GetProjectionType() == SceneCamera::ProjectionType::Orthographic)
				{
					float orthoSize = camera.GetOrthographicSize();
					bool bOrthoSizeChanged = false;
					DrawLabeledControl("Size", [&]() { bOrthoSizeChanged = ImGui::DragFloat("##OrthoSize", &orthoSize); });
					if (bOrthoSizeChanged)
						camera.SetOrthographicSize(orthoSize);

					float orthoNear = camera.GetOrthographicNearClip();
					bool bOrthoNearChanged = false;
					DrawLabeledControl("Near", [&]() { bOrthoNearChanged = ImGui::DragFloat("##OrthoNear", &orthoNear); });
					if (bOrthoNearChanged)
						camera.SetOrthographicNearClip(orthoNear);

					float orthoFar = camera.GetOrthographicFarClip();
					bool bOrthoFarChanged = false;
					DrawLabeledControl("Far", [&]() { bOrthoFarChanged = ImGui::DragFloat("##OrthoFar", &orthoFar); });
					if (bOrthoFarChanged)
						camera.SetOrthographicFarClip(orthoFar);

					DrawLabeledControl("Fixed Aspect Ratio", [&component]() { ImGui::Checkbox("##FixedAspect", &component.FixedAspectRatio); });
				}
			}, "CameraCompIcon");

		DrawComponent<FollowCameraComponent>("Follow Camera", entity, [entity](FollowCameraComponent& component)
			{
				Ref<Level> context = EditorLayer::GetPanel<SceneHierarchyPanel>()->GetContext();
				Entity targetEntity;
				if (context && component.bHasTarget)
					targetEntity = context->TryGetEntityByUUID(component.TargetUUID);

				static ImGuiTextFilter sTargetEntityFilter;

				const char* modeNames[] = {
					EditorLayer::GetText("ThirdPerson"),
					EditorLayer::GetText("TopDown")
				};
				int modeIndex = static_cast<int>(component.Mode);
				DrawLabeledControl("Mode", [&]()
					{
						if (ImGui::Combo("##FollowCameraMode", &modeIndex, modeNames, IM_ARRAYSIZE(modeNames)))
							component.Mode = static_cast<FollowCameraMode>(modeIndex);
					});

				std::string targetLabel = component.bHasTarget
					? (targetEntity ? EntityDisplayName(targetEntity) : std::string(EditorLayer::GetText("Missing")))
					: std::string(EditorLayer::GetText("None"));

				DrawLabeledControl("Target Entity", [&]()
					{
						ImGui::Button(targetLabel.c_str(), ImVec2(-1.0f, 0.0f));
						if (ImGui::BeginDragDropTarget())
						{
							if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayloadType))
							{
								UUID payloadUuid;
								if (TryReadEntityFromPayload(payload, payloadUuid))
								{
									component.TargetUUID = payloadUuid;
									component.bHasTarget = true;
									component.bRuntimeInitialized = false;
									MarkLastItemEditedIfChanged(true);
								}
							}
							ImGui::EndDragDropTarget();
						}
					});

				DrawLabeledControl("Target Search", [&]()
					{
						sTargetEntityFilter.Draw("##FollowTargetSearch", -1.0f);
					});

				DrawLabeledControl("Target Picker", [&]()
					{
						if (!context)
						{
							ImGui::BeginDisabled();
						ImGui::Button(EditorLayer::GetText("No Scene Context"), ImVec2(-1.0f, 0.0f));
							ImGui::EndDisabled();
							return;
						}

						if (ImGui::BeginCombo("##FollowTargetPicker", targetLabel.c_str()))
						{
							for (auto candidateHandle : context->mRegistry.view<IDComponent>())
							{
								Entity candidate = { candidateHandle, context.get() };
								if (!candidate || candidate == entity)
									continue;

								const std::string candidateName = EntityDisplayName(candidate);
								if (!sTargetEntityFilter.PassFilter(candidateName.c_str()))
									continue;

								const UUID candidateUUID = candidate.UUID();
								const bool bSelected = component.bHasTarget && candidateUUID == component.TargetUUID;
								ImGui::PushID(candidateUUID.str().c_str());
								if (ImGui::Selectable(candidateName.c_str(), bSelected))
								{
									component.TargetUUID = candidateUUID;
									component.bHasTarget = true;
									component.bRuntimeInitialized = false;
									MarkLastItemEditedIfChanged(true);
								}
								if (bSelected)
									ImGui::SetItemDefaultFocus();
								ImGui::PopID();
							}
							ImGui::EndCombo();
						}
					});

				DrawLabeledControl("Target Actions", [&]()
					{
						if (ImGui::Button(EditorLayer::GetText("Use Selected")))
						{
							Entity selected = EditorLayer::GetPanel<SceneHierarchyPanel>()->GetSelectedEntity();
							if (selected)
							{
								component.TargetUUID = selected.UUID();
								component.bHasTarget = true;
								component.bRuntimeInitialized = false;
								MarkLastItemEditedIfChanged(true);
							}
						}
						ImGui::SameLine();
						if (ImGui::Button(EditorLayer::GetText("Clear")))
						{
							component.bHasTarget = false;
							component.bRuntimeInitialized = false;
							MarkLastItemEditedIfChanged(true);
						}
					});

				DrawLabeledControl("Input Yaw", [&component]() { ImGui::Checkbox("##FollowAllowYaw", &component.bAllowYawInput); });
				DrawLabeledControl("Input Pitch", [&component]() { ImGui::Checkbox("##FollowAllowPitch", &component.bAllowPitchInput); });
				DrawLabeledControl("Hold RMB", [&component]() { ImGui::Checkbox("##FollowHoldRmb", &component.bRequireHoldToRotate); });
				DrawLabeledControl("Follow Rotation", [&component]() { ImGui::Checkbox("##FollowTargetRot", &component.bFollowTargetRotation); });
				DrawLabeledControl("Mouse Sensitivity", [&component]() { ImGui::DragFloat("##FollowMouseSense", &component.MouseSensitivity, 0.0005f, 0.0005f, 0.05f, "%.4f"); });

				if (component.Mode == FollowCameraMode::TopDown)
				{
					DrawLabeledControl("TopDown Yaw", [&component]() { ImGui::DragFloat("##TopDownYaw", &component.TopDownYawDegrees, 0.1f, -180.0f, 180.0f, "%.1f"); });
					DrawLabeledControl("TopDown Pitch", [&component]() { ImGui::DragFloat("##TopDownPitch", &component.TopDownPitchDegrees, 0.1f, 5.0f, 89.0f, "%.1f"); });
					DrawLabeledControl("TopDown Distance", [&component]() { ImGui::DragFloat("##TopDownDistance", &component.TopDownDistance, 0.05f, 0.1f, 100.0f, "%.2f"); });
				}
			}, "CameraCompIcon");

		DrawComponent<SpringArmComponent>("Spring Arm", entity, [](SpringArmComponent& component)
			{
				DrawVec3Control("Socket Offset", component.SocketOffset);
				DrawVec3Control("Target Offset", component.TargetOffset);
				DrawLabeledControl("Arm Length", [&component]() { ImGui::DragFloat("##SpringArmLength", &component.ArmLength, 0.05f, 0.1f, 50.0f, "%.2f"); });
				DrawLabeledControl("Min Arm Length", [&component]() { ImGui::DragFloat("##SpringArmMinLength", &component.MinArmLength, 0.01f, 0.05f, 20.0f, "%.2f"); });
				DrawLabeledControl("Enable Lag", [&component]() { ImGui::Checkbox("##SpringArmLag", &component.bEnableLag); });
				DrawLabeledControl("Lag Speed", [&component]() { ImGui::DragFloat("##SpringArmLagSpeed", &component.LagSpeed, 0.1f, 0.1f, 60.0f, "%.2f"); });
				DrawLabeledControl("Enable Collision", [&component]() { ImGui::Checkbox("##SpringArmColl", &component.bEnableCollision); });
				DrawLabeledControl("Probe Radius", [&component]() { ImGui::DragFloat("##SpringArmProbe", &component.ProbeRadius, 0.01f, 0.01f, 2.0f, "%.2f"); });
				DrawLabeledControl("TopDown Collision", [&component]() { ImGui::Checkbox("##SpringArmTopDownColl", &component.bEnableTopDownCollision); });
				DrawLabeledControl("Collision Snap", [&component]() { ImGui::DragFloat("##SpringArmSnapSpeed", &component.CollisionSnapSpeed, 0.1f, 0.1f, 80.0f, "%.2f"); });
				DrawLabeledControl("Collision Recover", [&component]() { ImGui::DragFloat("##SpringArmRecoverSpeed", &component.CollisionRecoverSpeed, 0.1f, 0.1f, 80.0f, "%.2f"); });
			}, "CameraCompIcon");

		DrawComponent<CharacterMovementComponent>("Character Movement", entity, [](CharacterMovementComponent& component)
			{
				DrawLabeledControl("Max Speed", [&component]() { ImGui::DragFloat("##MoveMaxSpeed", &component.MaxSpeed, 0.05f, 0.0f, 20.0f, "%.2f"); });
				DrawLabeledControl("Enable Sprint", [&component]() { ImGui::Checkbox("##MoveEnableSprint", &component.bEnableSprint); });
				DrawLabeledControl("Sprint Mult", [&component]() { ImGui::DragFloat("##MoveSprintMult", &component.SprintSpeedMultiplier, 0.01f, 1.0f, 5.0f, "%.2f"); });
				DrawLabeledControl("Sprint Accel", [&component]() { ImGui::DragFloat("##MoveSprintAccel", &component.SprintAccelerationMultiplier, 0.01f, 1.0f, 3.0f, "%.2f"); });
				DrawLabeledControl("Acceleration", [&component]() { ImGui::DragFloat("##MoveAccel", &component.Acceleration, 0.1f, 0.0f, 100.0f, "%.2f"); });
				DrawLabeledControl("Deceleration", [&component]() { ImGui::DragFloat("##MoveDecel", &component.Deceleration, 0.1f, 0.0f, 100.0f, "%.2f"); });
				DrawLabeledControl("Turn Rate", [&component]() { ImGui::DragFloat("##MoveTurnRate", &component.TurnRate, 0.1f, 0.0f, 40.0f, "%.2f"); });
				DrawLabeledControl("Use World Up", [&component]() { ImGui::Checkbox("##MoveUseWorldUp", &component.bUseWorldUp); });
				DrawLabeledControl("Orient To Move", [&component]() { ImGui::Checkbox("##MoveOrient", &component.bOrientRotationToMovement); });
				DrawLabeledControl("Max Slope Angle", [&component]() { ImGui::DragFloat("##MoveMaxSlope", &component.MaxSlopeAngleDegrees, 0.1f, 0.0f, 89.0f, "%.1f"); });
				DrawLabeledControl("Step Up Height", [&component]() { ImGui::DragFloat("##MoveStepUp", &component.StepUpHeight, 0.01f, 0.0f, 2.0f, "%.2f"); });
				DrawLabeledControl("Stick To Floor", [&component]() { ImGui::DragFloat("##MoveStickFloor", &component.StickToFloorDistance, 0.01f, 0.0f, 2.0f, "%.2f"); });
				DrawLabeledControl("Gravity", [&component]() { ImGui::DragFloat("##MoveGravity", &component.Gravity, 0.1f, -50.0f, 0.0f, "%.2f"); });
			}, "CharacterCompIcon");

		DrawComponent<SpriteRendererComponent>("Sprite Render", entity, [](auto& component)
			{
				DrawLabeledControl("Color", [&component]() { ImGui::ColorEdit4("##SpriteColor", glm::value_ptr(component.Color)); });

				DrawLabeledControl("Texture", [&component]()
					{
						const char* label = component.Texture.empty()
							? EditorLayer::GetText("Drop Texture")
							: component.Texture.c_str();
						ImGui::Button(label, ImVec2(-1.0f, 0.0f));
						if (ImGui::BeginDragDropTarget())
						{
							if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
							{
								const wchar_t* path = (const wchar_t*)payload->Data;
								const std::filesystem::path relativePath(path);
								const std::string assetPath = Utils::NormalizeAssetPath(
									(std::filesystem::path("Assets") / relativePath).generic_string());
								const std::filesystem::path texturePath = ConfigManager::GetFullPath(assetPath);
								std::string extension = texturePath.extension().string();
								std::transform(extension.begin(), extension.end(), extension.begin(),
									[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
								const bool bTextureFile = extension == ".png" || extension == ".jpg" ||
									extension == ".jpeg" || extension == ".tga" || extension == ".bmp" ||
									extension == ".hdr" || extension == ".dds";
								if (bTextureFile && std::filesystem::is_regular_file(texturePath))
								{
									component.Texture = assetPath;
									MarkLastItemEditedIfChanged(true);
								}
								else
									HE_WARN("Unsupported or missing texture asset {0}", texturePath.string());
							}
							ImGui::EndDragDropTarget();
						}
					});

				DrawLabeledControl("Tiling Factor", [&component]() { ImGui::DragFloat("##SpriteTiling", &component.TilingFactor, 0.1f, 0.0f, 100.0f); });
			}, "SpriteCompIcon");

		DrawComponent<CircleRendererComponent>("Circle Render", entity, [](auto& component)
			{
				DrawLabeledControl("Color", [&component]() { ImGui::ColorEdit4("##CircleColor", glm::value_ptr(component.Color)); });
				DrawLabeledControl("Thickness", [&component]() { ImGui::DragFloat("##CircleThickness", &component.Thickness, 0.025f, 0.0f, 1.0f); });
				DrawLabeledControl("Fade", [&component]() { ImGui::DragFloat("##CircleFade", &component.Fade, 0.00025f, 0.0f, 1.0f); });
			}, "CircleCompIcon");

		DrawComponent<Rigidbody2DComponent>("Rigidbody 2D", entity, [](auto& component)
			{
				const char* bodyTypeStrings[] = { "Static", "Dynamic", "Kinematic" };
				const char* currentBodyTypeString = bodyTypeStrings[(int)component.Type];
				DrawLabeledControl("Body Type", [&]()
					{
						if (ImGui::BeginCombo("##RigidBody2DType", EditorLayer::GetText(currentBodyTypeString)))
						{
							for (int i = 0; i < 3; i++)
							{
								bool isSelected = component.Type == (Rigidbody2DComponent::BodyType)i;
								if (ImGui::Selectable(EditorLayer::GetText(bodyTypeStrings[i]), isSelected))
								{
									currentBodyTypeString = bodyTypeStrings[i];
									component.Type = (Rigidbody2DComponent::BodyType)i;
								}
								if (isSelected)
									ImGui::SetItemDefaultFocus();
							}
							ImGui::EndCombo();
						}
					});

				DrawLabeledControl("Fixed Rotation", [&component]() { ImGui::Checkbox("##RigidBody2DFixedRotation", &component.FixedRotation); });
			}, "RigidbodyCompIcon");

		DrawComponent<BoxCollider2DComponent>("Box Collider 2D", entity, [](auto& component)
			{
				DrawLabeledControl("Offset", [&component]() { ImGui::DragFloat2("##BoxColliderOffset", glm::value_ptr(component.Offset)); });
				DrawLabeledControl("Size", [&component]() { ImGui::DragFloat2("##BoxColliderSize", glm::value_ptr(component.Size)); });
				DrawLabeledControl("Density", [&component]() { ImGui::DragFloat("##BoxColliderDensity", &component.Density, 0.01f, 0.0f, 1.0f); });
				DrawLabeledControl("Friction", [&component]() { ImGui::DragFloat("##BoxColliderFriction", &component.Friction, 0.01f, 0.0f, 1.0f); });
				DrawLabeledControl("Restitution", [&component]() { ImGui::DragFloat("##BoxColliderRestitution", &component.Restitution, 0.01f, 0.0f, 1.0f); });
				DrawLabeledControl("Restitution Threshold", [&component]() { ImGui::DragFloat("##BoxColliderRestitutionThreshold", &component.RestitutionThreshold, 0.01f, 0.0f); });
			}, "ColliderCompIcon");

		DrawComponent<CircleCollider2DComponent>("Circle Collider 2D", entity, [](auto& component)
			{
				DrawLabeledControl("Offset", [&component]() { ImGui::DragFloat2("##CircleColliderOffset", glm::value_ptr(component.Offset)); });
				DrawLabeledControl("Radius", [&component]() { ImGui::DragFloat("##CircleColliderRadius", &component.Radius); });
				DrawLabeledControl("Density", [&component]() { ImGui::DragFloat("##CircleColliderDensity", &component.Density, 0.01f, 0.0f, 1.0f); });
				DrawLabeledControl("Friction", [&component]() { ImGui::DragFloat("##CircleColliderFriction", &component.Friction, 0.01f, 0.0f, 1.0f); });
				DrawLabeledControl("Restitution", [&component]() { ImGui::DragFloat("##CircleColliderRestitution", &component.Restitution, 0.01f, 0.0f, 1.0f); });
				DrawLabeledControl("Restitution Threshold", [&component]() { ImGui::DragFloat("##CircleColliderRestitutionThreshold", &component.RestitutionThreshold, 0.01f, 0.0f); });
			}, "ColliderCompIcon");

		DrawComponent<CapsuleCollider3DComponent>("Capsule Collider 3D", entity, [](CapsuleCollider3DComponent& component)
			{
				DrawVec3Control("Center", component.Center);
				DrawLabeledControl("Radius", [&component]() { ImGui::DragFloat("##CapsuleRadius", &component.Radius, 0.01f, 0.01f, 5.0f, "%.2f"); }, 170.0f);
				DrawLabeledControl("Height", [&component]() { ImGui::DragFloat("##CapsuleHeight", &component.Height, 0.01f, 0.05f, 20.0f, "%.2f"); }, 170.0f);
				DrawLabeledControl("Is Trigger", [&component]() { ImGui::Checkbox("##CapsuleIsTrigger", &component.IsTrigger); }, 170.0f);
			}, "ColliderCompIcon");

		const SceneHierarchyPanel::MeshSelectionContext consumedMeshSelectionContext =
			EditorLayer::GetPanel<SceneHierarchyPanel>()->ConsumeMeshSelectionContext();

		DrawComponent<MeshComponent>("Mesh Renderer", entity, [this, entity, consumedMeshSelectionContext](MeshComponent& component)
			{
				const SceneHierarchyPanel::MeshSelectionContext& selectionContext = consumedMeshSelectionContext;

				const bool bUseAutoExpand = selectionContext.bValid &&
					selectionContext.MaterialIndex < component.mMesh->mMaterial.size() &&
					selectionContext.SubMeshIndex < component.mMesh->mSubMeshes.size();

				DrawLabeledControl("Mesh Path", [&]()
					{
						std::string standardPath = Utils::NormalizeAssetPath(component.Path);
						ImGui::Text(std::string_view(standardPath.c_str() + standardPath.find_last_of("/") + 1, standardPath.length()).data());
						if (ImGui::BeginDragDropTarget())
						{
							if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
							{
								auto path = (const wchar_t*)payload->Data;
								component.Path = Utils::NormalizeAssetPath((std::filesystem::path("Assets") / path).string());
								component.mMesh = CreateRef<Mesh>(component.Path);
								component.MaterialOverrides.clear();
								component.mAuthoredMaterials.clear();
								MarkLastItemEditedIfChanged(true);
							}
							ImGui::EndDragDropTarget();
						}

						ImGui::SameLine();
						if (ImGui::Button("...##MeshPath"))
						{
							std::string filepath = FileDialogs::OpenFile("Model (*.obj;*.fbx;*.dae;*.gltf)\0*.obj;*.fbx;*.dae;*.gltf\0").string();
							if (filepath.find("Assets") != std::string::npos)
								filepath = Utils::NormalizeAssetPath(filepath.substr(filepath.find("Assets"), filepath.length()));
							if (!filepath.empty())
							{
									component.mMesh = CreateRef<Mesh>(filepath);
									component.Path = filepath;
									component.MaterialOverrides.clear();
									component.mAuthoredMaterials.clear();
									MarkLastItemEditedIfChanged(true);
							}
						}
					}, ImGuiWrapper::kWidePropertyLabelWidth);

				if (bUseAutoExpand && selectionContext.bExpandMaterialTree)
				{
					ImGui::SetNextItemOpen(true, ImGuiCond_Always);
				}

				if (ImGuiWrapper::TreeNodeExStyle2((void*)"Material", EditorLayer::GetText("Materials")))
				{
					for (uint32_t matIndex = 0; matIndex < component.mMesh->mMaterial.size(); ++matIndex)
					{
						auto& material = component.mMesh->mMaterial[matIndex];
						const bool bSelectedMaterial = bUseAutoExpand && matIndex == selectionContext.MaterialIndex;
						std::string label = std::string("material") + std::to_string(matIndex);
						ImGui::PushID(label.c_str());
						const std::string materialName = material ? std::filesystem::path(material->mMatResPath).stem().string() : std::string();
						const bool bCanOpenMaterial = material && IsOpenableMaterialPath(material->mMatResPath);
						const bool bMissingMaterial = material && !bCanOpenMaterial;

						if (bSelectedMaterial && selectionContext.bExpandMaterialTree)
						{
							ImGui::SetNextItemOpen(true, ImGuiCond_Always);
						}

						std::string slotTitle = std::string(EditorLayer::GetText("Slot")) + " " + std::to_string(matIndex) + " - " +
							(material ? (materialName.empty() ? EditorLayer::GetText("(Unnamed Material)") : materialName) : EditorLayer::GetText("(Empty Material Slot)"));

						const ImGuiTreeNodeFlags slotFlags = ImGuiTreeNodeFlags_Framed |
							ImGuiTreeNodeFlags_SpanAvailWidth |
							ImGuiTreeNodeFlags_FramePadding;

						ImVec4 slotHeader = ImVec4(0.15f, 0.19f, 0.26f, 0.60f);
						ImVec4 slotHeaderHovered = ImVec4(0.20f, 0.27f, 0.36f, 0.75f);
						ImVec4 slotHeaderActive = ImVec4(0.22f, 0.31f, 0.42f, 0.85f);
						if (bMissingMaterial)
						{
							slotHeader = ImVec4(0.30f, 0.16f, 0.16f, 0.56f);
							slotHeaderHovered = ImVec4(0.40f, 0.20f, 0.20f, 0.72f);
							slotHeaderActive = ImVec4(0.48f, 0.24f, 0.24f, 0.82f);
						}
						else if (!material)
						{
							slotHeader = ImVec4(0.19f, 0.20f, 0.22f, 0.55f);
							slotHeaderHovered = ImVec4(0.24f, 0.25f, 0.29f, 0.70f);
							slotHeaderActive = ImVec4(0.28f, 0.30f, 0.34f, 0.82f);
						}

						ImGui::PushStyleColor(ImGuiCol_Header, slotHeader);
						ImGui::PushStyleColor(ImGuiCol_HeaderHovered, slotHeaderHovered);
						ImGui::PushStyleColor(ImGuiCol_HeaderActive, slotHeaderActive);
						const bool bSlotOpen = ImGui::TreeNodeEx("SlotHeader", slotFlags, "%s", slotTitle.c_str());
						ImGui::PopStyleColor(3);

						ImVec2 headerMin = ImGui::GetItemRectMin();
						ImVec2 headerMax = ImGui::GetItemRectMax();
						const bool bHeaderHovered = ImGui::IsItemHovered();
						if (bHeaderHovered)
						{
							ImU32 hoverOutline = ImGui::GetColorU32(ImVec4(0.48f, 0.66f, 0.95f, 0.88f));
							ImGui::GetWindowDrawList()->AddRect(headerMin, headerMax, hoverOutline, 6.0f, 0, 1.4f);
						}

						const char* statusText = EditorLayer::GetText(bCanOpenMaterial ? "Bound" : (bMissingMaterial ? "Missing" : "Empty"));
						ImVec4 statusColor = bCanOpenMaterial ? ImVec4(0.36f, 0.86f, 0.56f, 0.98f) : (bMissingMaterial ? ImVec4(0.95f, 0.45f, 0.45f, 0.98f) : ImVec4(0.82f, 0.82f, 0.82f, 0.95f));
						float statusWidth = ImGui::CalcTextSize(statusText).x;
						ImGui::GetWindowDrawList()->AddText(
							ImVec2(headerMax.x - statusWidth - ImGuiWrapper::S(16.0f), headerMin.y + ImGuiWrapper::S(4.0f)),
							ImGui::GetColorU32(statusColor),
							statusText);

						if (bCanOpenMaterial && bHeaderHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
						{
							mParent->CreateMaterialPanel(material->mMatResPath);
						}

						if (bSlotOpen)
						{
							if (bSelectedMaterial)
							{
								const std::string pickedSubMeshText = std::string(EditorLayer::GetText("Picked SubMesh")) + ": " + std::to_string(selectionContext.SubMeshIndex);
								ImGui::TextColored(ImVec4(0.27f, 0.75f, 0.37f, 1.0f), "%s", pickedSubMeshText.c_str());
							}

							if (material)
							{
								ImGui::TextDisabled("%s: %s", EditorLayer::GetText("Asset"), FileNameFromPath(material->mMatResPath).c_str());
								ImGui::TextDisabled("%s: %s", EditorLayer::GetText("Shader"), material->GetShaderPath().empty() ? EditorLayer::GetText("(None)") : material->GetShaderPath().c_str());
							}

							ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.17f, 0.22f, 0.30f, 0.85f));
							ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.30f, 0.42f, 0.95f));
							ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.26f, 0.36f, 1.0f));
							const char* dropText = EditorLayer::GetText(material ? "Drop .hmat Here To Replace" : "Drop .hmat Here");
							ImGui::Button(dropText, ImVec2(-1.0f, ImGuiWrapper::S(34.0f)));
							ImGui::PopStyleColor(3);

							if (ImGui::BeginDragDropTarget())
							{
								if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
								{
									auto path = (const wchar_t*)payload->Data;
									std::string materialPath = ToRootRelativePath(std::filesystem::path(path));
									if (IsMaterialAssetPath(materialPath))
									{
										if (mParent) mParent->QueueSceneOperations(Json::array({{{"op","material"},
                                            {"entity",Entity(entity).UUID().str()},{"slot",matIndex},{"path",materialPath}}}));
										MarkLastItemEditedIfChanged(true);
									}
								}
								ImGui::EndDragDropTarget();
							}

							if (ImGui::BeginTable("MaterialActions", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadInnerX))
							{
								ImGui::TableNextRow();

								ImGui::TableSetColumnIndex(0);
								if (!bCanOpenMaterial)
									ImGui::BeginDisabled();
								if (ImGui::Button(EditorLayer::GetText("Open Material"), ImVec2(-FLT_MIN, ImGuiWrapper::S(30.0f))))
								{
									mParent->CreateMaterialPanel(material->mMatResPath);
								}
								if (!bCanOpenMaterial)
									ImGui::EndDisabled();

								ImGui::TableSetColumnIndex(1);
								if (ImGui::Button(EditorLayer::GetText("Pick..."), ImVec2(-FLT_MIN, ImGuiWrapper::S(30.0f))))
								{
									std::string filepath = FileDialogs::OpenFile("Material (*.hmat)\0*.hmat\0").string();
									if (filepath.find("Assets") != std::string::npos)
										filepath = filepath.substr(filepath.find("Assets"));
									if (!filepath.empty())
									{
										if (mParent) mParent->QueueSceneOperations(Json::array({{{"op","material"},
                                            {"entity",Entity(entity).UUID().str()},{"slot",matIndex},{"path",Utils::NormalizeAssetPath(filepath)}}}));
										MarkLastItemEditedIfChanged(true);
									}
								}

								ImGui::EndTable();
							}

							ImGui::TreePop();
						}

						ImGui::Spacing();

						ImGui::PopID();
					}

					ImGui::TreePop();
				}

						}, "MeshCompIcon");

					DrawComponent<AnimatorComponent>("Animator", entity, [this](AnimatorComponent& component)
						{
			DrawLabeledControl("Controller", [&]()
				{
					std::string ctrlPath = Utils::NormalizeAssetPath(component.controllerPath);
					auto ctrlLastSlash = ctrlPath.find_last_of('/');
					std::string ctrlName = (ctrlLastSlash != std::string::npos) ? ctrlPath.substr(ctrlLastSlash + 1) : ctrlPath;
					const bool bCanOpenController = IsOpenableAnimControllerPath(ctrlPath);
					ImGui::TextUnformatted(ctrlName.empty() ? EditorLayer::GetText("None") : ctrlName.c_str());
					if (ImGui::BeginDragDropTarget())
					{
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
						{
							auto path = (const wchar_t*)payload->Data;
							std::string value = Utils::NormalizeAssetPath((std::filesystem::path("Assets") / path).string());
							if (std::filesystem::path(value).extension() == ".hanimctrl")
							{
								component.SetControllerPath(value);
								MarkLastItemEditedIfChanged(true);
							}
						}
						ImGui::EndDragDropTarget();
					}
					ImGui::SameLine();
					if (ImGui::Button("...##AnimCtrl"))
					{
						std::string filepath = FileDialogs::OpenFile("Animation Controller (*.hanimctrl)\0*.hanimctrl\0\0").string();
						if (filepath.find("Assets") != std::string::npos)
							filepath = Utils::NormalizeAssetPath(filepath.substr(filepath.find("Assets"), filepath.length()));
						if (!filepath.empty())
						{
							component.SetControllerPath(filepath);
							MarkLastItemEditedIfChanged(true);
						}
					}
					ImGui::SameLine();
					if (!bCanOpenController)
						ImGui::BeginDisabled();
					const std::string openControllerLabel = std::string(EditorLayer::GetText("Open")) + "##AnimCtrl";
					if (ImGui::Button(openControllerLabel.c_str()))
					{
						mParent->CreateAnimControllerPanel(ctrlPath);
					}
					if (!bCanOpenController)
						ImGui::EndDisabled();
				}, ImGuiWrapper::kWidePropertyLabelWidth);

								// --- Clip 选择器（运行时加载后才显示）---
							if (component.bLoaded && component.GetClipCount() > 0)
							{
								DrawLabeledControl("Clip", [&]()
									{
										const bool bControllerOwnsClip = !component.controllerPath.empty() && component.controllerPath != "None";
										const std::string currentClipName = component.GetCurrentClipIndex() >= 0
											? component.GetClipName(component.GetCurrentClipIndex())
											: std::string("None");
										if (bControllerOwnsClip) ImGui::BeginDisabled();
										if (ImGui::BeginCombo("##Clip", currentClipName.c_str()))
										{
											bool noneSelected = (component.currentClipIndex < 0);
									if (ImGui::Selectable(EditorLayer::GetText("None"), noneSelected))
									{
										component.currentClipIndex = -1;
										MarkLastItemEditedIfChanged(true);
									}
											if (noneSelected)
												ImGui::SetItemDefaultFocus();

											for (int i = 0; i < component.GetClipCount(); ++i)
											{
												bool selected = (i == component.currentClipIndex);
												std::string clipName = component.GetClipName(i);
										if (ImGui::Selectable(clipName.c_str(), selected))
										{
											component.currentClipIndex = i;
											MarkLastItemEditedIfChanged(true);
										}
												if (selected)
													ImGui::SetItemDefaultFocus();
											}
											ImGui::EndCombo();
										}
										if (bControllerOwnsClip) ImGui::EndDisabled();
									});
								ImGui::ProgressBar(component.GetProgress(), ImVec2(-1.0f, 0.0f));
							}

							// --- 播放控制 ---
							DrawLabeledControl("Play", [&component]() { ImGui::Checkbox("##bPlay", &component.bPlay); });
							DrawLabeledControl("Loop", [&component]() { ImGui::Checkbox("##bLoop", &component.bLoop); });
							DrawLabeledControl("Speed", [&component]() { ImGui::SliderFloat("##AnimSpeed", &component.playSpeed, 0.01f, 5.0f, "%.2f"); });
							DrawLabeledControl("Root Motion", [&component]() { ImGui::Checkbox("##AnimRootMotion", &component.bEnableRootMotion); });
							DrawLabeledControl("Lock Root XZ", [&component]()
								{
									if (!component.bEnableRootMotion)
										ImGui::BeginDisabled();
									ImGui::Checkbox("##AnimRootMotionXZ", &component.bLockRootMotionXZ);
									if (!component.bEnableRootMotion)
										ImGui::EndDisabled();
								});
						}, "AnimatorCompIcon");

		DrawComponent<Rigidbody3DComponent>("Rigidbody 3D", entity, [](auto& component)
			{
				const char* bodyTypeStrings[] = { "Static", "Dynamic", "Kinematic" };
				const char* currentBodyTypeString = bodyTypeStrings[(int)component.Type];

				DrawLabeledControl("Body Type", [&]()
					{
						if (ImGui::BeginCombo("##BodyType", EditorLayer::GetText(currentBodyTypeString)))
						{
							for (int i = 0; i < 3; i++)
							{
								bool isSelected = component.Type == (Rigidbody3DComponent::Body3DType)i;
								if (ImGui::Selectable(EditorLayer::GetText(bodyTypeStrings[i]), isSelected))
								{
									currentBodyTypeString = bodyTypeStrings[i];
									component.Type = (Rigidbody3DComponent::Body3DType)i;
								}

								if (isSelected)
									ImGui::SetItemDefaultFocus();
							}

							ImGui::EndCombo();
						}
					}, ImGuiWrapper::kWidePropertyLabelWidth);

				DrawLabeledControl("Collision Shape", [&]()
					{
				constexpr auto collisionShapes = magic_enum::enum_values<CollisionShape>();
				const std::string currentShapeName(magic_enum::enum_name(component.Shape));
				if (ImGui::BeginCombo("##Collision Shape", EditorLayer::GetText(currentShapeName.c_str())))
				{
					for (auto& shape : collisionShapes)
					{
						bool isSelected = component.Shape == shape;
						const std::string shapeName(magic_enum::enum_name(shape));
						if (ImGui::Selectable(EditorLayer::GetText(shapeName.c_str()), isSelected))
						{
							component.Shape = shape;
						}

						if (isSelected)
							ImGui::SetItemDefaultFocus();
					}

					ImGui::EndCombo();
				}
					}, ImGuiWrapper::kWidePropertyLabelWidth);

				if (component.Shape != CollisionShape::None)
				{
					DrawVec3Control("Collider Offset", component.ColliderOffset, 0.0f, 170.0f);
					DrawVec3Control("Collider Rotation", component.ColliderRotation, 0.0f, 170.0f);
					DrawVec3Control("Collider Scale", component.ColliderScale, 1.0f, 170.0f);

					const auto& floatValueUI = [](const char* name, const char* id, float& value) {
						DrawLabeledControl(name, [&]() { ImGui::SliderFloat(id, &value, 0.0f, 1.0f, "%.2f"); }, ImGuiWrapper::kWidePropertyLabelWidth);
					};

					floatValueUI("linearDamping", "##rb3d_linearDamping", component.linearDamping);
					floatValueUI("angularDamping", "##rb3d_angularDamping", component.angularDamping);
					floatValueUI("restitution", "##rb3d_restitution", component.restitution);
					floatValueUI("friction", "##rb3d_friction", component.friction);

					DrawLabeledControl("isTrigger", [&component = component]() { ImGui::Checkbox("##rb3d_isTrigger", &component.IsTrigger); }, ImGuiWrapper::kWidePropertyLabelWidth);
					DrawLabeledControl("enableCCD", [&component = component]() { ImGui::Checkbox("##rb3d_enableCCD", &component.EnableCCD); }, ImGuiWrapper::kWidePropertyLabelWidth);
					DrawLabeledControl("allowSleeping", [&component = component]() { ImGui::Checkbox("##rb3d_allowSleeping", &component.AllowSleeping); }, ImGuiWrapper::kWidePropertyLabelWidth);
				}

				DrawLabeledControl("mass", [&component = component]() { ImGui::SliderFloat("##masas", &component.mass, 0.0f, 10.0f, "%.2f"); }, ImGuiWrapper::kWidePropertyLabelWidth);
			}, "RigidbodyCompIcon");

		DrawComponent<PointLightComponent>("Point Light", entity, [](auto& component)
			{
				DrawLightIntensity(component.Intensity, &component.IntensityUnits);
				
				DrawLabeledControl("Light Color", [&component = component]() { ImGui::ColorEdit3("##LightColor", (float*)&component.LightColor); });
				DrawLabeledControl("Radius (m)", [&component = component]() { ImGui::DragFloat("##PointLightRadius", &component.Radius, 0.1f, 0.01f, FLT_MAX, "%.3g", ImGuiSliderFlags_AlwaysClamp); });
			}, "PointLightCompIcon");

		DrawComponent<SpotLightComponent>("Spot Light", entity, [](auto& component)
			{
				DrawLightIntensity(component.Intensity, &component.IntensityUnits, component.OuterConeAngle);
				DrawLabeledControl("Light Color", [&]() { ImGui::ColorEdit3("##SpotColor", glm::value_ptr(component.LightColor)); });
				DrawLabeledControl("Radius (m)", [&]() { ImGui::DragFloat("##SpotRadius", &component.Radius, 0.1f, 0.01f, FLT_MAX, "%.3g", ImGuiSliderFlags_AlwaysClamp); });
				DrawLabeledControl("Outer Half Angle", [&]() { ImGui::DragFloat("##SpotOuter", &component.OuterConeAngle, 0.1f, 0.1f, 89.0f, "%.2f deg", ImGuiSliderFlags_AlwaysClamp); });
				component.InnerConeAngle = std::min(component.InnerConeAngle, component.OuterConeAngle);
				DrawLabeledControl("Inner Half Angle", [&]() { ImGui::DragFloat("##SpotInner", &component.InnerConeAngle, 0.1f, 0.0f, component.OuterConeAngle, "%.2f deg", ImGuiSliderFlags_AlwaysClamp); });
			}, "PointLightCompIcon");

		DrawComponent<DirectionalLightComponent>("Directional Light", entity, [](auto& component)
			{
				DrawLightIntensity(component.Intensity);
				DrawLabeledControl("Light Color", [&component = component]() { ImGui::ColorEdit3("##DirLightColor", (float*)&component.Color); });
				DrawLabeledControl("Local Direction", [&component = component]() { ImGui::DragFloat3("##DirLightLocalDirection", glm::value_ptr(component.LocalDirection), 0.01f, -1.0f, 1.0f, "%.2f"); });
				DrawLabeledControl("Cast Shadows", [&component = component]() { ImGui::Checkbox("##DirLightCastShadows", &component.bCastShadows); });
				DrawLabeledControl("Cascade Count", [&component = component]()
					{
						ImGui::SliderInt("##DirLightCascadeCount", &component.CascadeCount, 1, 4);
						component.CascadeCount = std::clamp(component.CascadeCount, 1, 4);
					});
				DrawLabeledControl("Shadow Map Size", [&component = component]()
					{
						ImGui::SliderInt("##DirLightShadowMapSize", &component.ShadowMapSize, 256, 4096);
						component.ShadowMapSize = std::clamp(component.ShadowMapSize, 256, 8192);
					});
				DrawLabeledControl("Shadow Distance", [&component = component]() { ImGui::DragFloat("##DirLightShadowDistance", &component.ShadowDistance, 1.0f, 1.0f, 1000.0f, "%.1f"); });
				DrawLabeledControl("Cascade Lambda", [&component = component]() { ImGui::SliderFloat("##DirLightCascadeLambda", &component.CascadeLambda, 0.0f, 1.0f, "%.2f"); });
				DrawLabeledControl("Depth Bias", [&component = component]() { ImGui::DragFloat("##DirLightDepthBias", &component.DepthBias, 0.0001f, 0.0f, 0.02f, "%.5f"); });
				DrawLabeledControl("Normal Bias", [&component = component]() { ImGui::DragFloat("##DirLightNormalBias", &component.NormalBias, 0.001f, 0.0f, 1.0f, "%.3f"); });
				DrawLabeledControl("Use PCF", [&component = component]() { ImGui::Checkbox("##DirLightUsePCF", &component.bUsePCF); });
				DrawLabeledControl("PCF Radius", [&component = component]() { ImGui::SliderFloat("##DirLightPCFRadius", &component.PCFRadius, 0.0f, 4.0f, "%.2f"); });
				DrawLabeledControl("Shadow Strength", [&component = component]() { ImGui::SliderFloat("##DirLightShadowStrength", &component.ShadowStrength, 0.0f, 1.0f, "%.2f"); });
			}, "DirLightCompIcon");

		DrawComponent<BakedIrradianceVolumeComponent>("Baked Irradiance Volume", entity, [](auto& component)
			{
				DrawLabeledControl("Enabled", [&]() { ImGui::Checkbox("##BakedEnabled", &component.bEnabled); });
				DrawLabeledControl("World Bounds Min", [&]() { ImGui::DragFloat3("##BakedMin", glm::value_ptr(component.BoundsMin), 0.05f); }, ImGuiWrapper::kWidePropertyLabelWidth);
				DrawLabeledControl("World Bounds Max", [&]() { ImGui::DragFloat3("##BakedMax", glm::value_ptr(component.BoundsMax), 0.05f); }, ImGuiWrapper::kWidePropertyLabelWidth);
				DrawLabeledControl("Blend Distance", [&]() { ImGui::DragFloat("##BakedBlend", &component.BlendDistance, 0.05f, 0.001f, 1000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp); });
				DrawLabeledControl("Intensity", [&]() { ImGui::DragFloat("##BakedIntensity", &component.Intensity, 0.01f, 0.0f, 1000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp); });
				ImGui::TextWrapped("World-space lighting data. Moving the entity does not move or rotate the bake. Rebuild after changing geometry or lights.");
				if (component.IrradianceSH.size() != 9 || !glm::all(glm::greaterThan(component.BoundsMax, component.BoundsMin)))
					ImGui::TextWrapped("No valid bake: nine SH coefficients and positive bounds are required. The renderer uses environment lighting as fallback.");
				else
					ImGui::TextUnformatted("Baked data: 9 RGB SH coefficients");
			}, "DirLightCompIcon");

		DrawComponent<ReflectionProbeComponent>("Reflection Probe", entity, [](auto& component)
			{
				DrawLabeledControl("Enabled", [&]() { ImGui::Checkbox("##ProbeEnabled", &component.bEnabled); });
				DrawLabeledControl("HDR Texture", [&]()
					{
						const auto name = component.HdrTexturePath.empty() ? std::string("Drop .hdr file") : FileNameFromPath(component.HdrTexturePath);
						ImGui::Button(name.c_str(), ImVec2(-1.0f, 0.0f));
						if (ImGui::BeginDragDropTarget())
						{
							if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
							{
								const auto candidate = ToRootRelativePath(std::filesystem::path(static_cast<const wchar_t*>(payload->Data)));
								auto extension = std::filesystem::path(candidate).extension().string();
								std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
								if (extension == ".hdr")
								{
									component.HdrTexturePath = Utils::NormalizeAssetPath(candidate);
									MarkLastItemEditedIfChanged(true);
								}
							}
							ImGui::EndDragDropTarget();
						}
					}, ImGuiWrapper::kWidePropertyLabelWidth);
				DrawLabeledControl("Box Half Extents", [&]() { ImGui::DragFloat3("##ProbeExtents", glm::value_ptr(component.BoxHalfExtents), 0.05f, 0.001f, 10000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp); }, ImGuiWrapper::kWidePropertyLabelWidth);
				DrawLabeledControl("Capture Offset", [&]() { ImGui::DragFloat3("##ProbeOffset", glm::value_ptr(component.CaptureOffset), 0.05f); });
				DrawLabeledControl("Blend Distance", [&]() { ImGui::DragFloat("##ProbeBlend", &component.BlendDistance, 0.05f, 0.001f, 1000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp); });
				DrawLabeledControl("Intensity", [&]() { ImGui::DragFloat("##ProbeIntensity", &component.Intensity, 0.01f, 0.0f, 1000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp); });
				DrawLabeledControl("Affect Diffuse", [&]() { ImGui::Checkbox("##ProbeDiffuse", &component.bAffectDiffuse); });
				ImGui::TextWrapped("Disable Affect Diffuse when a baked irradiance volume supplies the same region's diffuse light.");
			}, "DirLightCompIcon");

		DrawComponent<EnvironmentLightComponent>("Environment Light", entity, [](auto& component)
			{
				DrawLabeledControl("HDR Texture", [&component = component]()
					{
						const std::string currentPath = Utils::NormalizeAssetPath(component.HdrTexturePath);
						const std::string displayName = currentPath.empty() ? std::string("Drop .hdr file") : FileNameFromPath(currentPath);
						ImGui::Button(displayName.c_str(), ImVec2(-1.0f, 0.0f));
						if (ImGui::BeginDragDropTarget())
						{
							if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
							{
								auto path = (const wchar_t*)payload->Data;
								std::string candidate = ToRootRelativePath(std::filesystem::path(path));
								std::string ext = std::filesystem::path(candidate).extension().string();
								std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
								if (ext == ".hdr")
								{
									component.HdrTexturePath = Utils::NormalizeAssetPath(candidate);
									MarkLastItemEditedIfChanged(true);
								}
							}
							ImGui::EndDragDropTarget();
						}

						ImGui::SameLine();
						if (ImGui::Button("...##EnvironmentHdrPath"))
						{
							std::string filepath = FileDialogs::OpenFile("HDR (*.hdr)\0*.hdr\0\0").string();
							if (!filepath.empty())
							{
								std::filesystem::path selectedPath(filepath);
								if (selectedPath.is_absolute())
								{
									const std::filesystem::path rootPath = ConfigManager::GetInstance().GetRootFolder();
									if (selectedPath.string().find(rootPath.string()) != std::string::npos)
									{
										std::filesystem::path relPath = std::filesystem::relative(selectedPath, rootPath);
										component.HdrTexturePath = Utils::NormalizeAssetPath(relPath.string());
										MarkLastItemEditedIfChanged(true);
									}
								}
								else if (selectedPath.string().find("Assets") != std::string::npos)
								{
									component.HdrTexturePath = Utils::NormalizeAssetPath(selectedPath.string());
									MarkLastItemEditedIfChanged(true);
								}
							}
						}
					}, ImGuiWrapper::kWidePropertyLabelWidth);

				DrawLabeledControl("Exposure (EV100)", [&]() { ImGui::DragFloat("##EnvExposureEV", &component.ExposureEV100, 0.05f, -24.0f, 32.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp); });
				DrawLabeledControl("Exposure Compensation", [&]() { ImGui::DragFloat("##EnvExposureCompensation", &component.ExposureCompensation, 0.05f, -24.0f, 24.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp); }, ImGuiWrapper::kWidePropertyLabelWidth);
				DrawLabeledControl("IBL Intensity", [&component = component]() { ImGui::DragFloat("##EnvIBLIntensity", &component.IBLIntensity, std::max(0.01f, component.IBLIntensity * 0.01f), 0.0f, FLT_MAX, "%.6g", ImGuiSliderFlags_AlwaysClamp); });
				DrawLabeledControl("Sky Tint", [&]() { ImGui::ColorEdit3("##EnvSkyTint", &component.SkyColorTint.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float); });
				DrawLabeledControl("Distance Fog", [&]() { ImGui::Checkbox("##EnvFogEnabled", &component.bFogEnabled); });
				if (component.bFogEnabled)
				{
					DrawLabeledControl("Fog Color", [&]() { ImGui::ColorEdit3("##EnvFogColor", &component.FogColor.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float); });
					DrawLabeledControl("Fog Density", [&]() { ImGui::DragFloat("##EnvFogDensity", &component.FogDensity, 0.0001f, 0.0f, 2.0f, "%.4f", ImGuiSliderFlags_AlwaysClamp); });
					DrawLabeledControl("Fog Start Distance", [&]() { ImGui::DragFloat("##EnvFogStart", &component.FogStartDistance, 0.1f, 0.0f, 10000.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp); }, ImGuiWrapper::kWidePropertyLabelWidth);
					DrawLabeledControl("Fog Base Height", [&]() { ImGui::DragFloat("##EnvFogBase", &component.FogBaseHeight, 0.1f, -10000.0f, 10000.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp); }, ImGuiWrapper::kWidePropertyLabelWidth);
					DrawLabeledControl("Fog Height Falloff", [&]() { ImGui::DragFloat("##EnvFogFalloff", &component.FogHeightFalloff, 0.0001f, 0.0f, 10.0f, "%.4f", ImGuiSliderFlags_AlwaysClamp); }, ImGuiWrapper::kWidePropertyLabelWidth);
					DrawLabeledControl("Fog Sunward Color", [&]() { ImGui::ColorEdit3("##EnvFogSunColor", &component.FogDirectionalColor.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float); }, ImGuiWrapper::kWidePropertyLabelWidth);
					DrawLabeledControl("Fog Sunward Intensity", [&]() { ImGui::DragFloat("##EnvFogSunIntensity", &component.FogDirectionalIntensity, 0.01f, 0.0f, 16.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp); }, ImGuiWrapper::kWidePropertyLabelWidth);
					DrawLabeledControl("Fog Sunward Exponent", [&]() { ImGui::DragFloat("##EnvFogSunExponent", &component.FogDirectionalExponent, 0.1f, 1.0f, 64.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp); }, ImGuiWrapper::kWidePropertyLabelWidth);
				}
				DrawLabeledControl("Secondary Fog Density", [&]() { ImGui::DragFloat("##EnvSecondaryFogDensity", &component.SecondaryFogDensity, 0.0001f, 0.0f, 2.0f, "%.4f", ImGuiSliderFlags_AlwaysClamp); }, ImGuiWrapper::kWidePropertyLabelWidth);
				if (component.SecondaryFogDensity > 0.0f)
				{
					DrawLabeledControl("Secondary Fog Color", [&]() { ImGui::ColorEdit3("##EnvSecondaryFogColor", &component.SecondaryFogColor.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float); }, ImGuiWrapper::kWidePropertyLabelWidth);
					DrawLabeledControl("Secondary Fog Start", [&]() { ImGui::DragFloat("##EnvSecondaryFogStart", &component.SecondaryFogStartDistance, 0.1f, 0.0f, 10000.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp); }, ImGuiWrapper::kWidePropertyLabelWidth);
					DrawLabeledControl("Secondary Fog Base", [&]() { ImGui::DragFloat("##EnvSecondaryFogBase", &component.SecondaryFogBaseHeight, 0.1f, -10000.0f, 10000.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp); }, ImGuiWrapper::kWidePropertyLabelWidth);
					DrawLabeledControl("Secondary Fog Falloff", [&]() { ImGui::DragFloat("##EnvSecondaryFogFalloff", &component.SecondaryFogHeightFalloff, 0.0001f, 0.0f, 10.0f, "%.4f", ImGuiSliderFlags_AlwaysClamp); }, ImGuiWrapper::kWidePropertyLabelWidth);
				}
				DrawLabeledControl("Clouds Enabled", [&]() { ImGui::Checkbox("##CloudEnabled", &component.bCloudsEnabled); });
				if (component.bCloudsEnabled)
				{
					DrawLabeledControl("Cloud Bottom", [&]() { ImGui::DragFloat("##CloudBottom", &component.CloudBottom, 1.0f, -10000.0f, 10000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp); });
					DrawLabeledControl("Cloud Thickness", [&]() { ImGui::DragFloat("##CloudThickness", &component.CloudThickness, 1.0f, 1.0f, 2000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp); });
					DrawLabeledControl("Cloud MaxDistance", [&]() { ImGui::DragFloat("##CloudMaxDistance", &component.CloudMaxDistance, 10.0f, 10.0f, 20000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp); });
					DrawLabeledControl("Cloud Coverage", [&]() { ImGui::DragFloat("##CloudCoverage", &component.CloudCoverage, 0.01f, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp); });
					DrawLabeledControl("Cloud Density", [&]() { ImGui::DragFloat("##CloudDensity", &component.CloudDensity, 0.001f, 0.0f, 0.2f, "%.3f", ImGuiSliderFlags_AlwaysClamp); });
					DrawLabeledControl("Cloud Scale", [&]() { ImGui::DragFloat("##CloudScale", &component.CloudScale, 1.0f, 10.0f, 4000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp); });
					DrawLabeledControl("Cloud Roundness", [&]() { ImGui::DragFloat("##CloudRoundness", &component.CloudRoundness, 0.01f, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp); });
					DrawLabeledControl("Cloud Erosion", [&]() { ImGui::DragFloat("##CloudErosion", &component.CloudErosion, 0.01f, 0.0f, 0.5f, "%.3f", ImGuiSliderFlags_AlwaysClamp); });
					DrawLabeledControl("Cloud WindAngle", [&]() { ImGui::DragFloat("##CloudWindAngle", &component.CloudWindAngle, 1.0f, -360.0f, 360.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp); });
					DrawLabeledControl("Cloud WindSpeed", [&]() { ImGui::DragFloat("##CloudWindSpeed", &component.CloudWindSpeed, 0.1f, 0.0f, 100.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp); });
					DrawLabeledControl("Cloud EvolutionSpeed", [&]() { ImGui::DragFloat("##CloudEvolutionSpeed", &component.CloudEvolutionSpeed, 0.001f, 0.0f, 0.5f, "%.3f", ImGuiSliderFlags_AlwaysClamp); });
					DrawLabeledControl("Cloud Ambient", [&]() { ImGui::DragFloat("##CloudAmbient", &component.CloudAmbient, 0.01f, 0.0f, 2.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp); });
					DrawLabeledControl("Cloud SilverLining", [&]() { ImGui::DragFloat("##CloudSilverLining", &component.CloudSilverLining, 0.01f, 0.0f, 2.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp); });
					DrawLabeledControl("Cloud Seed", [&]() { ImGui::InputScalar("##CloudSeed", ImGuiDataType_U32, &component.CloudSeed); });
					DrawLabeledControl("Cloud LitColor", [&]() { ImGui::ColorEdit3("##CloudLitColor", &component.CloudLitColor.x); });
					DrawLabeledControl("Cloud ShadowColor", [&]() { ImGui::ColorEdit3("##CloudShadowColor", &component.CloudShadowColor.x); });
				}
			}, "DirLightCompIcon");

		DrawComponent<PostProcessingComponent>("Post Processing", entity, [](auto& component)
			{
				DrawLabeledControl("Enabled", [&component]() { ImGui::Checkbox("##PostEnabled", &component.bEnabled); });
				DrawLabeledControl("Global", [&component]() { ImGui::Checkbox("##PostGlobal", &component.bGlobal); });
				if (component.bGlobal)
					ImGui::BeginDisabled();
				DrawLabeledControl("Local Volume", [&component]()
					{
						ImGui::Checkbox("##PostLocalVolume", &component.bLocalVolumeEnabled);
					});
				if (component.bGlobal)
					ImGui::EndDisabled();

				DrawLabeledControl("Priority", [&component]()
					{
						ImGui::DragFloat("##PostPriority", &component.Priority, 0.1f, -1000.0f, 1000.0f, "%.1f");
					});
				const bool bLocalBlend = !component.bGlobal && component.bLocalVolumeEnabled;
				if (!bLocalBlend)
					ImGui::BeginDisabled();
				DrawLabeledControl("Blend Radius", [&component]()
					{
						ImGui::DragFloat("##PostBlendRadius", &component.BlendRadius, 0.1f, 0.0f, 10000.0f, "%.2f");
					});
				if (!bLocalBlend)
					ImGui::EndDisabled();
				DrawLabeledControl("Blend Weight", [&component]()
					{
						ImGui::SliderFloat("##PostBlendWeight", &component.BlendWeight, 0.0f, 1.0f, "%.2f");
					});
				DrawLabeledControl("Override Exposure", [&component]()
					{
						ImGui::Checkbox("##PostOverrideExposure", &component.bOverrideExposure);
					});
				if (!component.bOverrideExposure)
					ImGui::BeginDisabled();
				DrawLabeledControl("Exposure", [&component]()
					{
						ImGui::DragFloat("##PostExposure", &component.ExposureEV100, 0.05f, -24.0f, 32.0f, "%.2f EV100", ImGuiSliderFlags_AlwaysClamp);
					});
				DrawLabeledControl("Exposure Compensation", [&component]() { ImGui::DragFloat("##PostExposureCompensation", &component.ExposureCompensation, 0.05f, -24.0f, 24.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp); }, ImGuiWrapper::kWidePropertyLabelWidth);
				if (!component.bOverrideExposure)
					ImGui::EndDisabled();
				DrawLabeledControl("Override Bloom", [&component]() { ImGui::Checkbox("##PostOverrideBloom", &component.bOverrideBloomIntensity); });
				if (!component.bOverrideBloomIntensity) ImGui::BeginDisabled();
				DrawLabeledControl("Bloom Intensity", [&component]() { ImGui::DragFloat("##PostBloom", &component.BloomIntensity, 0.01f, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp); });
				if (!component.bOverrideBloomIntensity) ImGui::EndDisabled();
				DrawLabeledControl("Override Contrast", [&component]() { ImGui::Checkbox("##PostOverrideContrast", &component.bOverrideContrast); });
				if (!component.bOverrideContrast) ImGui::BeginDisabled();
				DrawLabeledControl("Contrast", [&component]() { ImGui::DragFloat("##PostContrast", &component.Contrast, 0.01f, 0.1f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp); });
				if (!component.bOverrideContrast) ImGui::EndDisabled();
				DrawLabeledControl("Override Saturation", [&component]() { ImGui::Checkbox("##PostOverrideSaturation", &component.bOverrideSaturation); });
				if (!component.bOverrideSaturation) ImGui::BeginDisabled();
				DrawLabeledControl("Saturation", [&component]() { ImGui::DragFloat("##PostSaturation", &component.Saturation, 0.01f, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp); });
				if (!component.bOverrideSaturation) ImGui::EndDisabled();
				DrawLabeledControl("Override Temperature", [&component]() { ImGui::Checkbox("##PostOverrideTemperature", &component.bOverrideTemperature); });
				if (!component.bOverrideTemperature) ImGui::BeginDisabled();
				DrawLabeledControl("Temperature", [&component]() { ImGui::DragFloat("##PostTemperature", &component.Temperature, 0.01f, -1.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp); });
				if (!component.bOverrideTemperature) ImGui::EndDisabled();
				DrawLabeledControl("Override Vignette", [&component]() { ImGui::Checkbox("##PostOverrideVignette", &component.bOverrideVignetteIntensity); });
				if (!component.bOverrideVignetteIntensity) ImGui::BeginDisabled();
				DrawLabeledControl("Vignette Intensity", [&component]() { ImGui::DragFloat("##PostVignette", &component.VignetteIntensity, 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp); });
				if (!component.bOverrideVignetteIntensity) ImGui::EndDisabled();
				DrawLabeledControl("Override Color Gain", [&component]() { ImGui::Checkbox("##PostOverrideColorGain", &component.bOverrideColorGain); });
				if (!component.bOverrideColorGain) ImGui::BeginDisabled();
				DrawLabeledControl("Color Gain", [&component]() { ImGui::DragFloat3("##PostColorGain", &component.ColorGain.x, 0.01f, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp); });
				if (!component.bOverrideColorGain) ImGui::EndDisabled();
				DrawLabeledControl("Override Color Offset", [&component]() { ImGui::Checkbox("##PostOverrideColorOffset", &component.bOverrideColorOffset); });
				if (!component.bOverrideColorOffset) ImGui::BeginDisabled();
				DrawLabeledControl("Color Offset", [&component]() { ImGui::DragFloat3("##PostColorOffset", &component.ColorOffset.x, 0.001f, -0.25f, 0.25f, "%.3f", ImGuiSliderFlags_AlwaysClamp); });
				if (!component.bOverrideColorOffset) ImGui::EndDisabled();

				if (component.bGlobal)
				{
					ImGui::TextWrapped("%s", EditorLayer::GetText("Global post processing is applied to the entire scene."));
				}
				else if (component.bLocalVolumeEnabled)
				{
					ImGui::TextWrapped("%s", EditorLayer::GetText("The Transform defines a local post-process box; Blend Radius fades outside its boundary."));
				}
				else
				{
					ImGui::TextWrapped("%s", EditorLayer::GetText("Local Volume is explicitly disabled, preserving legacy bGlobal=false behavior."));
				}
			}, "DirLightCompIcon");

		DrawComponent<ScriptComponent>("Script", entity, [&entity](auto& component)
			{
                auto edited = component;
                if (!DrawScriptInspector(edited, entity)) return;
                if (ModeManager::IsEditState())
                {
                    Json patch;
                    Serializer::Serialize(patch, edited);
                    if (auto* editor = EditorLayer::GetPanel<PropertiesPanel>()->GetParent())
                        editor->QueueSceneOperations(Json::array({{{"op", "patch"}, {"entity", entity.UUID().str()}, {"component", "ScriptComponent"}, {"patch", patch}}}));
                }
                else component = std::move(edited);
			}, "ScriptCompIcon");

		DrawComponent<PythonScriptComponent>("Python Script", entity, [&entity](auto& component)
			{
				if (!ScriptEngine::IsAvailable())
					ImGui::TextWrapped("Python plugin he.script.python is unavailable. The script binding is preserved.");
				DrawLabeledControl("Script Name", [&]()
					{
						// component.Path must be end with .py
						auto pos = component.Path.find_last_of('.', component.Path.length() - 4);
						pos = (pos == std::string::npos ? 0 : pos + 1);
						ImGui::TextUnformatted(component.Path.c_str() + pos);
						if (ImGui::BeginDragDropTarget())
						{
							if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
							{
								auto path = (const wchar_t*)payload->Data;
								std::string pathStr = Utils::WStringToAnsi(path);
								auto pos = pathStr.find_first_of("Scripts");
								HE_CORE_ASSERT(pos != std::string::npos, "Not from Scripts folder!");
								component.Path = pathStr.substr(pos, -1);

								Utils::Replace(component.Path, "\\", ".");
								Utils::Replace(component.Path, "/", ".");

								MarkLastItemEditedIfChanged(true);
							}
							ImGui::EndDragDropTarget();
						}

						ImGui::SameLine();
						if (ImGui::Button("...##PyScript"))
						{
							std::string filepath = FileDialogs::OpenFile("PythonScript (*.py)\0*.py\0\0").string();
							if (!filepath.empty())
							{
								filepath = std::regex_replace(filepath, std::regex("\\\\"), "/");
								filepath = filepath.substr(filepath.find_last_of("/") + 1, filepath.length());
								component.Path = filepath;
								MarkLastItemEditedIfChanged(true);
							}
						}
					}, ImGuiWrapper::kWidePropertyLabelWidth);

					}, "ScriptCompIcon");

				DrawComponent<SoundComponent>("Sound", entity, [](auto& component)
					{
						DrawLabeledControl("Sound Path", [&]()
							{
								const std::string displayName = FileNameFromPath(Utils::NormalizeAssetPath(component.Path));
								ImGui::TextUnformatted(displayName.c_str());
								if (ImGui::BeginDragDropTarget())
								{
									if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
									{
										const auto* path = static_cast<const wchar_t*>(payload->Data);
										const std::filesystem::path selectedPath = ConfigManager::GetInstance().GetAssetsFolder() / path;
										std::string assetPath;
										if (TryMakeProjectAudioAssetPath(selectedPath, assetPath))
										{
											component.Path = std::move(assetPath);
											MarkLastItemEditedIfChanged(true);
										}
										else
										{
											HE_CORE_WARN("Sound asset must be WAV, MP3, or FLAC under project Assets: {0}", selectedPath.string());
										}
									}
									ImGui::EndDragDropTarget();
								}

								ImGui::SameLine();
								if (ImGui::Button("...##SoundPath"))
								{
									const std::filesystem::path selectedPath = FileDialogs::OpenFile(
										"Sound (*.wav;*.mp3;*.flac)\0*.wav;*.mp3;*.flac\0\0",
										ConfigManager::GetInstance().GetAssetsFolder().string());
									if (!selectedPath.empty())
									{
										std::string assetPath;
										if (TryMakeProjectAudioAssetPath(selectedPath, assetPath))
										{
											component.Path = std::move(assetPath);
											MarkLastItemEditedIfChanged(true);
										}
										else
										{
											HE_CORE_WARN("Sound asset must be WAV, MP3, or FLAC under project Assets: {0}", selectedPath.string());
										}
									}
								}
							}, ImGuiWrapper::kWidePropertyLabelWidth);

						DrawLabeledControl("Play On Start", [&component]() { ImGui::Checkbox("##SoundPlayOnStart", &component.Play); });
						DrawLabeledControl("Loop", [&component]() { ImGui::Checkbox("##SoundLoop", &component.bLooping); });
						DrawLabeledControl("Spatialized", [&component]() { ImGui::Checkbox("##SoundSpatialized", &component.bSpatialized); });
						DrawLabeledControl("Stream", [&component]() { ImGui::Checkbox("##SoundStream", &component.bStream); });
						DrawLabeledControl("Volume", [&component]() { ImGui::SliderFloat("##SoundVolume", &component.Volume, 0.0f, 1.0f); });
						DrawLabeledControl("Pitch", [&component]() { ImGui::DragFloat("##SoundPitch", &component.Pitch, 0.01f, 0.01f, 4.0f, "%.2f"); });

						DrawLabeledControl("Bus", [&component]()
							{
								const char* busNames[] = {
									EditorLayer::GetText("Music"),
									EditorLayer::GetText("Gameplay"),
									EditorLayer::GetText("UI")
								};
								int busIndex = static_cast<int>(component.Bus);
								if (ImGui::Combo("##SoundBus", &busIndex, busNames, IM_ARRAYSIZE(busNames)))
									component.Bus = static_cast<AudioBus>(busIndex);
							});

						if (component.bSpatialized)
						{
							DrawLabeledControl("Attenuation", [&component]()
								{
									const char* attenuationNames[] = {
										EditorLayer::GetText("None"),
										EditorLayer::GetText("Inverse"),
										EditorLayer::GetText("Linear"),
										EditorLayer::GetText("Exponential")
									};
									int attenuationIndex = static_cast<int>(component.Attenuation);
									if (attenuationIndex < 0 || attenuationIndex >= IM_ARRAYSIZE(attenuationNames))
										attenuationIndex = static_cast<int>(AudioAttenuationModel::Inverse);
									if (ImGui::Combo("##SoundAttenuation", &attenuationIndex, attenuationNames, IM_ARRAYSIZE(attenuationNames)))
										component.Attenuation = static_cast<AudioAttenuationModel>(attenuationIndex);
								});
							DrawLabeledControl("Min Distance", [&component]() { ImGui::DragFloat("##SoundMinDistance", &component.MinDistance, 0.1f, 0.01f, component.MaxDistance, "%.2f"); });
							DrawLabeledControl("Max Distance", [&component]() { ImGui::DragFloat("##SoundMaxDistance", &component.MaxDistance, 0.1f, component.MinDistance, 1000.0f, "%.2f"); });
							DrawLabeledControl("Rolloff", [&component]() { ImGui::DragFloat("##SoundRolloff", &component.Rolloff, 0.05f, 0.0f, 10.0f, "%.2f"); });
							DrawLabeledControl("Doppler", [&component]() { ImGui::DragFloat("##SoundDoppler", &component.DopplerFactor, 0.05f, 0.0f, 10.0f, "%.2f"); });
						}
			}, "SoundCompIcon");

				DrawComponent<AudioListenerComponent>("Audio Listener", entity, [](auto& component)
					{
						DrawLabeledControl("Active", [&component]() { ImGui::Checkbox("##AudioListenerActive", &component.bActive); });
						DrawLabeledControl("Camera Orientation", [&component]() { ImGui::Checkbox("##AudioListenerCameraOrientation", &component.bUsePrimaryCameraOrientation); });
					}, "SoundCompIcon");

		ImGui::PopStyleColor(5);
		ImGui::PopStyleVar(3);
	}

	void PropertiesPanel::OnImGuiRender()
	{
		if (!(*pOpen))
			return;

		ImGuiWrapper::SetNextWindowSizeConstraints(ImVec2(320.0f, 300.0f), ImVec2(FLT_MAX, FLT_MAX));
		if (mFirstOpen)
		{
			ImGuiWrapper::SetNextWindowSize(ImVec2(420.0f, 760.0f), ImGuiCond_Once);
			mFirstOpen = false;
		}

		const std::string localizedWindowName = std::string(EditorLayer::GetText(mName.c_str())) + "###" + mName;
		if (!ImGui::Begin(localizedWindowName.c_str(), pOpen))
		{
			ImGui::End();
			return;
		}

		if (IEditorUndoClient* undoClient = GetUndoClient())
		{
			mParent->NotifyUndoClientActivity(
				undoClient,
				ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows),
				ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows));
		}

		OnImGuiRenderImpl();
		ImGui::End();
	}

	IEditorUndoClient* PropertiesPanel::GetUndoClient()
	{
		return mParent ? mParent->GetSceneUndoClient() : nullptr;
	}

	void PropertiesPanel::OnImGuiRenderImpl()
	{
        std::erase_if(sScriptSelection, [](const Entity& entity) { return !entity; });
        if (sScriptSelection.size() > 1)
        {
            ImGui::Text("Script selection: %zu entities", sScriptSelection.size());
            if (!ModeManager::IsEditState()) { ImGui::TextWrapped("Stop Play to edit multiple authoring objects."); return; }
            if (!sSelectionContext || !sSelectionContext.HasComponent<ScriptComponent>())
            { ImGui::TextWrapped("Multi-edit requires compatible Script components on all selected entities."); return; }
            const auto& original = sSelectionContext.GetComponent<ScriptComponent>();
            std::vector<ScriptFields> selectedFields;
            try
            {
                for (auto entity : sScriptSelection)
                {
                    if (!entity.HasComponent<ScriptComponent>() || EditorLayer::GetPanel<SceneHierarchyPanel>()->IsEntityLocked(entity))
                    { ImGui::TextWrapped("Selection contains a locked entity or an entity without Script."); return; }
                    const auto& script = entity.GetComponent<ScriptComponent>();
                    if (script.Backend != original.Backend || script.Type != original.Type)
                    { ImGui::TextWrapped("Selected script types/backends differ. Select matching types to edit fields together."); return; }
                    selectedFields.push_back(DecodeScriptFields(script.Fields));
                }
                auto edited = original;
                std::set<std::string> touched;
                bool resetPerformed = false;
                if (DrawScriptInspector(edited, sSelectionContext, selectedFields, &touched, &resetPerformed) && mParent)
                {
                    const auto updated = DecodeScriptFields(edited.Fields);
                    const auto backend = ScriptBackendRegistry::Find(edited.Backend);
                    auto* validator = backend ? dynamic_cast<IScriptEditorFields*>(backend.get()) : nullptr;
                    Json operations = Json::array();
                    for (std::size_t index = 0; index < sScriptSelection.size(); ++index)
                    {
                        auto values = selectedFields[index];
                        for (const auto& id : touched) if (auto value = updated.find(id); value != updated.end()) values.insert_or_assign(id, value->second);
                        if (sScriptSelection[index] == sSelectionContext) values = updated;
                        else if (validator && !touched.empty()) values = validator->EditFields(edited.Type, values, resetPerformed);
                        operations.push_back({{"op", "patch"}, {"entity", sScriptSelection[index].UUID().str()}, {"component", "ScriptComponent"},
                            {"patch", {{"Backend", edited.Backend}, {"Type", edited.Type}, {"Enabled", edited.Enabled}, {"Fields", EncodeScriptFields(values)}}}});
                    }
                    mParent->QueueSceneOperations(std::move(operations));
                }
            }
            catch (const std::exception& error) { ImGui::TextWrapped("Multi-edit rejected; no fields changed: %s", error.what()); }
            return;
        }
		if (sSelectionContext)
		{
			ImGui::BeginGroup();
			DrawComponents(sSelectionContext);
			ImGui::EndGroup();

			if (ImGui::IsItemDeactivatedAfterEdit() && mParent)
				mParent->MarkSceneDirty("Edit Properties");
		}
	}
}
