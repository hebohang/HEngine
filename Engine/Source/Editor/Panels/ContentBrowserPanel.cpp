#include "ContentBrowserPanel.h"
#include "Editor/EditorLayer.h"
#include "Editor/Manager/IconManager.h"
#include "Editor/ImGuiWrapper/ImGuiWrapper.h"
#include "Runtime/Events/Event.h"
#include "Runtime/Events/KeyEvent.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Graphics/VFX/EffekseerPackage.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Resource/ResType/Mesh.h"
#include "Runtime/Resource/ResType/Material.h"
#include "Runtime/Resource/ResType/Skeleton.h"
#include "Runtime/Resource/ResType/AnimController.h"
#include "Runtime/Resource/ResType/UIRes.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeUiResource.h"
#include "Runtime/Utils/PlatformUtils.h"
#include "Runtime/Utils/StringUtils/StringUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "Runtime/Core/Modules/ModuleManager.h"
#include "Runtime/Core/Plugins/Services/MeshImporterService.h"
#include "Runtime/Profile/Profile.h"
#include "all_serializer.h"

#include <imgui/imgui.h>
#include <imgui-notify-v2/imgui_notify.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace HEngine
{

	namespace Utils
	{
		enum class EffekseerPreviewFormat
		{
			Editable,
			RuntimeOnly,
			Unreadable,
		};

		static EffekseerPreviewFormat GetEffekseerPreviewFormat(const std::filesystem::path& effectPath)
		{
			std::ifstream input(effectPath, std::ios::binary);
			std::array<char, 4> magic{};
			if (!input.read(magic.data(), static_cast<std::streamsize>(magic.size())))
				return EffekseerPreviewFormat::Unreadable;

			// SKFE is Effekseer's runtime export format. It contains no editor document,
			// so opening it in the authoring tool creates a dirty NewFile document.
			if (std::memcmp(magic.data(), "SKFE", magic.size()) == 0)
				return EffekseerPreviewFormat::RuntimeOnly;

			return EffekseerPreviewFormat::Editable;
		}

		static bool IsPathUnderRoot(const std::filesystem::path& path, const std::filesystem::path& root)
		{
			const std::string pathString = path.lexically_normal().generic_string();
			const std::string rootString = root.lexically_normal().generic_string();
			return pathString == rootString || pathString.rfind(rootString + "/", 0) == 0;
		}

		static std::filesystem::path GetEngineAssetsRoot()
		{
			return ConfigManager::GetEngineContentFullPath("Assets");
		}

		static bool IsReadOnlyContentPath(const std::filesystem::path& path)
		{
			const ConfigManager& config = ConfigManager::GetInstance();
			if (IsPathUnderRoot(path, config.GetEngineContentFolder()))
				return true;

			try
			{
				return ConfigManager::GetContentRootForPath(path) != config.GetRootFolder();
			}
			catch (const std::invalid_argument&)
			{
				return true;
			}
		}

		static std::filesystem::path GetContentRootForPath(const std::filesystem::path& path)
		{
			const auto& config = ConfigManager::GetInstance();
			const std::filesystem::path assetsRoot = config.GetAssetsFolder();
			const std::filesystem::path scriptsRoot = config.GetScriptsFolder();
			const std::filesystem::path shadersRoot = config.GetShadersFolder();
			const std::filesystem::path engineAssetsRoot = GetEngineAssetsRoot();

			if (IsPathUnderRoot(path, engineAssetsRoot))
				return engineAssetsRoot;
			if (IsPathUnderRoot(path, scriptsRoot))
				return scriptsRoot;
			if (IsPathUnderRoot(path, shadersRoot))
				return shadersRoot;
			return assetsRoot;
		}

		static std::string MakeRelativeBrowserPath(const std::filesystem::path& absolutePath)
		{
			const auto& config = ConfigManager::GetInstance();
			const std::filesystem::path assetsRoot = config.GetAssetsFolder();
			const std::filesystem::path engineAssetsRoot = GetEngineAssetsRoot();
			const std::filesystem::path rootPath = GetContentRootForPath(absolutePath);
			const std::filesystem::path relativePath = std::filesystem::relative(absolutePath, rootPath);

			if (rootPath == assetsRoot || rootPath == engineAssetsRoot)
				return relativePath.generic_string();

			const std::filesystem::path rootedRelativePath = rootPath.filename() / relativePath;
			return rootedRelativePath.generic_string();
		}

		static const std::unordered_map<std::string, std::string> iconExtToName = {
			{"mp3", "MusicIcon"},
			{"wav", "MusicIcon"},
			{"flac", "MusicIcon"},
			{"ogg", "MusicIcon"},
			{"hmesh", "MeshIcon"},
			{"hmat", "MaterialIcon"},
			{"hui", "UiIcon"},
			{"hprefab", "PrefabIcon"},
			{"efkpkg", "EffectIcon"},
			{"efkefc", "EffectIcon"},
			{"hbt", "BehaviorTreeIcon"},
			{"py", "ScriptIcon"},
			{"hlsl", "ShaderIcon"},
			{"hlsli", "ShaderIcon"},
			{"ttf", "FontIcon"},
			{"otf", "FontIcon"},
			{"rml", "UiMarkupIcon"},
			{"rcss", "UiMarkupIcon"},
			{"fbx", "MeshSourceIcon"},
			{"obj", "MeshSourceIcon"},
			{"gltf", "MeshSourceIcon"},
			{"glb", "MeshSourceIcon"},
			{"dae", "MeshSourceIcon"},
			{"blend", "MeshSourceIcon"},
			{"mtl", "MeshSourceIcon"},
			{"json", "ConfigIcon"},
			{"ini", "ConfigIcon"},
			{"hanimctrl", "AnimCtrlIcon"},
			{"hskel", "SkeletonIcon"},
			{"hanim", "AnimIcon"},
			{"he", "SceneIcon"},
		};

		static bool IsImageFormat(std::string extension)
		{
			if (extension == "png" || extension == "jpg" || extension == "jpeg" ||
				extension == "bmp" || extension == "hdr" || extension == "tga")
			{
				return true;
			}
			return false;
		}

		static bool ContainsCaseInsensitive(std::string_view text, std::string_view searchText)
		{
			if (searchText.empty())
				return true;

			return std::search(
				text.begin(),
				text.end(),
				searchText.begin(),
				searchText.end(),
				[](char lhs, char rhs)
				{
					return std::tolower(static_cast<unsigned char>(lhs)) ==
						std::tolower(static_cast<unsigned char>(rhs));
				}) != text.end();
		}
	}

	ContentBrowserPanel::ContentBrowserPanel(EditorLayer* parent, std::string panelName, bool* pOpen)
		: BasePanel(parent, panelName, pOpen),
		mCurrentDirectory(ConfigManager::GetInstance().GetAssetsFolder()),
		mSelectedDirectory(mCurrentDirectory)
	{
	}

	void ContentBrowserPanel::OnEvent(Event& e)
	{
		EventDispatcher dispatcher(e);
		if (bHandleTreeEvent)
		{
			dispatcher.Dispatch<KeyPressedEvent>(HE_BIND_EVENT_FN(ContentBrowserPanel::OnKeyPressedTree));
		}
		else if (bHandleContentEvent)
		{
			dispatcher.Dispatch<KeyPressedEvent>(HE_BIND_EVENT_FN(ContentBrowserPanel::OnKeyPressedContent));
		}
	}

	void ContentBrowserPanel::OnImGuiRenderImpl()
	{
		HE_PROFILE;

		ImGui::Columns(2);

		static bool init = true;
		static float sLastUIScale = -1.0f;
		if (init || ImGuiWrapper::HasScaleChanged(sLastUIScale))
		{
			ImGuiWrapper::SetColumnWidth(0, 240.0f);
			IconManager::GetInstance().ClearThumbnailCache();
			bTreeCacheDirty = true;
			init = false;
		}

		if (ImGui::BeginChild("CONTENT_BROWSER_TREE"))
		{
			HE_PROFILE_SECTION("Content Browser Tree");
			DrawTree();
			bHandleTreeEvent = ImGui::IsWindowFocused() && ImGui::IsWindowHovered();

			// Right-click on blank space
			if (ImGui::BeginPopupContextWindow(nullptr, ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
			{
				const std::filesystem::path& popupDirectory = mSelectedDirectory
					? *mSelectedDirectory
					: mCurrentDirectory;
				const bool bReadOnly = Utils::IsReadOnlyContentPath(popupDirectory);
				ImGuiWrapper::SeparatorWithText(EditorLayer::GetText("Basic"));
				if (bReadOnly)
				{
					ImGui::MenuItem(EditorLayer::GetText("Read-only"), nullptr, false, false);
				}
				else if (ImGui::MenuItem(EditorLayer::GetText("New Folder")))
				{
					NewFolder();
				}
				if (ImGui::MenuItem(EditorLayer::GetText("Show in Explorer")))
				{
					FileDialogs::OpenInExplorer(popupDirectory.string());
				}
				ImGui::EndPopup();
			}
		}
		ImGui::EndChild();

		ImGui::NextColumn();

		if (ImGui::BeginChild("CONTENT_BROWSER_CONTENT"))
		{
			HE_PROFILE_SECTION("Content Browser Content");
			DrawContent();
			bHandleContentEvent = ImGui::IsWindowFocused() && ImGui::IsWindowHovered();

			// Right-click on blank space
			if (ImGui::BeginPopupContextWindow(nullptr, ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
			{
				const bool bReadOnly = Utils::IsReadOnlyContentPath(mCurrentDirectory);
				if (bReadOnly)
				{
					ImGuiWrapper::SeparatorWithText(EditorLayer::GetText("Basic"));
					ImGui::MenuItem(EditorLayer::GetText("Read-only"), nullptr, false, false);
					if (ImGui::MenuItem(EditorLayer::GetText("Show in Explorer")))
						FileDialogs::OpenInExplorer(mCurrentDirectory.string());
					ImGui::BeginDisabled();
				}

				ImGuiWrapper::SeparatorWithText(EditorLayer::GetText("Import"));
				if (ImGui::MenuItem(EditorLayer::GetText("Import Asset")))
				{
					std::filesystem::path filepath = FileDialogs::OpenFile("", ConfigManager::GetInstance().GetAssetsFolder().string());  // TODO: Change Default path to project path

					if (!filepath.empty())
					{
						Ref<IMeshImporterService> meshImporter;
						ModuleManager& moduleManager = ModuleManager::GetInstance();
						if (moduleManager.IsPluginHostConfigured())
						{
							meshImporter = moduleManager.GetPluginManager()
								.GetServices()
								.TryGet<IMeshImporterService>(c_MeshImporterServiceId);
						}
						if (meshImporter && meshImporter->Import(filepath, mCurrentDirectory))
							InvalidateDirectoryCache();
						else
							HE_CORE_ERROR("ContentBrowserPanel: failed to import asset '{0}'", filepath.string());
					}
				}

				ImGuiWrapper::SeparatorWithText(EditorLayer::GetText("Folder"));
				if (ImGui::MenuItem(EditorLayer::GetText("New Folder")))
				{
					NewFolder();
				}

				ImGuiWrapper::SeparatorWithText(EditorLayer::GetText("Create Basic Asset"));
				if (ImGui::MenuItem(EditorLayer::GetText("New Material")))
				{
					if (!AssetManager::NewAsset<MaterialRes>("NewMat", mCurrentDirectory).empty())
						InvalidateDirectoryCache();
				}
				if (ImGui::MenuItem(EditorLayer::GetText("New Skeleton")))
				{
					SkeletonRes skel;
					skel.name = "NewSkeleton";
					auto out = AssetManager::GetAvailableFileName("NewSkeleton.hskel", mCurrentDirectory);
					SaveSkeletonRes(skel, out);
					InvalidateDirectoryCache();
				}
				if (ImGui::MenuItem(EditorLayer::GetText("New Anim Controller")))
				{
					AnimControllerRes ctrl;
					ctrl.name = "NewAnimController";
					ctrl.entryNodeId = 1;
					ctrl.nodes.push_back({ 1, "Entry", "Entry", { 60.0f, 120.0f }, "" });
					auto out = AssetManager::GetAvailableFileName("NewAnimController.hanimctrl", mCurrentDirectory);
					SaveAnimControllerRes(ctrl, out);
					InvalidateDirectoryCache();
				}
				if (ImGui::MenuItem(EditorLayer::GetText("New UI")))
				{
					UIRes uiRes;
					uiRes.Name = "NewUI";
					UINodeRes canvas;
					canvas.Id = 1;
					canvas.Type = UIWidgetType::Canvas;
					canvas.Name = "Canvas";
					canvas.ElementId = "canvas_root";
					canvas.Style.ClassName = "hui-canvas";
					canvas.Layout.AnchorMax = { 1.0f, 1.0f };
					canvas.Layout.OffsetMax = { 1280.0f, 720.0f };
					uiRes.Nodes.push_back(canvas);
					auto out = AssetManager::GetAvailableFileName("NewUI.hui", mCurrentDirectory);
					std::string error;
					try
					{
						const RuntimeUiPath path = ResolveRuntimeUiWrittenPath(
							ConfigManager::GetInstance().GetRootFolder(), out);
						if (SaveRuntimeUiDefinition(RuntimeResourceRegistry::GetInstance(), path, uiRes, &error))
							InvalidateDirectoryCache();
						else
							HE_CORE_ERROR("ContentBrowserPanel: failed to create UI resource '{0}', {1}", out.string(), error);
					}
					catch (const std::exception& exception)
					{
						HE_CORE_ERROR("ContentBrowserPanel: failed to create UI resource '{0}', {1}", out.string(), exception.what());
					}
				}

				if (bReadOnly)
					ImGui::EndDisabled();
				ImGui::EndPopup();
			}
		}
		ImGui::EndChild();

		ImGui::Columns(1);
	}

	void ContentBrowserPanel::DrawTree()
	{
		HE_PROFILE;

		if (bTreeCacheDirty)
			RefreshTreeCache();

		for (const DirectoryTreeNode& rootNode : mTreeRoots)
			DrawTreeNode(rootNode);
	}

	void ContentBrowserPanel::DrawTreeNode(const DirectoryTreeNode& node)
	{
		const ImGuiTreeNodeFlags baseFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
			ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_SpanFullWidth;

		ImGuiTreeNodeFlags nodeFlags = baseFlags;
		const std::string currentPathString = node.path.lexically_normal().generic_string();

		if (mSelectedDirectory.has_value())
		{
			const std::string strSelectedPath = mSelectedDirectory.value().lexically_normal().generic_string();

			if (currentPathString == strSelectedPath)
			{
				nodeFlags |= ImGuiTreeNodeFlags_Selected;
			}
			else if (strSelectedPath.size() > currentPathString.size() && strSelectedPath.rfind(currentPathString + "/", 0) == 0)
			{
				ImGui::SetNextItemOpen(true);
			}
		}

		const bool bHasChildren = !node.children.empty();
		if (!bHasChildren)
			nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

		std::string label = "##" + currentPathString;
		bool nodeOpen = ImGui::TreeNodeEx(label.c_str(), nodeFlags);

		if (ImGui::IsItemClicked())
		{
			mSelectedDirectory = node.path;
		}

		ImGuiWrapper::SameLine(0.0f);
		ImGui::Image(IconManager::ToImTextureID(IconManager::GetInstance().GetDirectoryIcon()), ImGuiWrapper::S(20.0f, 20.0f));
		ImGuiWrapper::SameLine(0.0f);
		ImGui::TextUnformatted(node.displayName.c_str());

		if (nodeOpen && bHasChildren)
		{
			for (const DirectoryTreeNode& childNode : node.children)
				DrawTreeNode(childNode);
			ImGui::TreePop();
		}
	}

	ContentBrowserPanel::DirectoryTreeNode ContentBrowserPanel::BuildTreeNode(const std::filesystem::path& currentPath) const
	{
		DirectoryTreeNode node;
		node.path = currentPath;
		node.displayName = Utils::PathToUtf8(currentPath.filename());
		if (node.displayName.empty())
			node.displayName = currentPath.lexically_normal().generic_string();

		std::vector<std::filesystem::path> childDirectories;
		std::error_code ec;
		try
		{
			for (const auto& directoryEntry : std::filesystem::directory_iterator(currentPath))
			{
				if (directoryEntry.is_directory(ec) && !ec)
					childDirectories.push_back(directoryEntry.path());
			}
		}
		catch (const std::filesystem::filesystem_error&)
		{
			return node;
		}

		std::sort(childDirectories.begin(), childDirectories.end(), [](const std::filesystem::path& lhs, const std::filesystem::path& rhs)
			{
				return lhs.filename().generic_string() < rhs.filename().generic_string();
			});

		node.children.reserve(childDirectories.size());
		for (const std::filesystem::path& childPath : childDirectories)
			node.children.push_back(BuildTreeNode(childPath));

		return node;
	}

	void ContentBrowserPanel::RefreshTreeCache()
	{
		HE_PROFILE;

		mTreeRoots.clear();

		const std::array<std::filesystem::path, 4> roots = {
			ConfigManager::GetInstance().GetAssetsFolder(),
			ConfigManager::GetInstance().GetScriptsFolder(),
			ConfigManager::GetInstance().GetShadersFolder(),
			Utils::GetEngineAssetsRoot()
		};

		for (const std::filesystem::path& rootPath : roots)
		{
			std::error_code ec;
			if (!std::filesystem::exists(rootPath, ec) || !std::filesystem::is_directory(rootPath, ec))
				continue;

			DirectoryTreeNode rootNode = BuildTreeNode(rootPath);
			if (rootPath == Utils::GetEngineAssetsRoot())
				rootNode.displayName = EditorLayer::GetText("Engine Content");
			mTreeRoots.push_back(std::move(rootNode));
		}

		bTreeCacheDirty = false;
	}

	void ContentBrowserPanel::DrawContent()
	{
		HE_PROFILE;

		if (!mSelectedDirectory)
		{
			return;
		}

		mCurrentDirectory = *mSelectedDirectory;
		if (mCurrentDirectory != mCachedDirectory || bDirCacheDirty)
		{
			HE_PROFILE_SECTION("Refresh Directory Cache");
			RefreshDirectoryCache();
		}
		MaterialThumbnailService* const materialThumbnails =
			mParent->GetMaterialThumbnailService();
		if (materialThumbnails)
			materialThumbnails->SetDirectoryGeneration(mThumbnailDirectoryGeneration);
		SceneThumbnailService* const sceneThumbnails =
			mParent->GetSceneThumbnailService();
		if (sceneThumbnails)
			sceneThumbnails->SetDirectoryGeneration(mThumbnailDirectoryGeneration);

		const std::filesystem::path assetsRoot = ConfigManager::GetInstance().GetAssetsFolder();
		const std::filesystem::path scriptsRoot = ConfigManager::GetInstance().GetScriptsFolder();
		const std::filesystem::path shadersRoot = ConfigManager::GetInstance().GetShadersFolder();
		const std::filesystem::path engineAssetsRoot = Utils::GetEngineAssetsRoot();

		std::filesystem::path breadcrumbRoot = assetsRoot;
		std::string breadcrumbRootLabel = EditorLayer::GetText("Assets");
		auto pickBreadcrumbRoot = [&](const std::filesystem::path& candidateRoot, const char* rootLabel)
		{
			const std::string currentPathString = mCurrentDirectory.lexically_normal().generic_string();
			const std::string rootPathString = candidateRoot.lexically_normal().generic_string();
			if (currentPathString == rootPathString || currentPathString.rfind(rootPathString + "/", 0) == 0)
			{
				breadcrumbRoot = candidateRoot;
				breadcrumbRootLabel = rootLabel;
				return true;
			}
			return false;
		};

		if (!pickBreadcrumbRoot(assetsRoot, EditorLayer::GetText("Assets")))
		{
			if (!pickBreadcrumbRoot(scriptsRoot, EditorLayer::GetText("Scripts")))
			{
				if (!pickBreadcrumbRoot(shadersRoot, EditorLayer::GetText("Shaders")))
					pickBreadcrumbRoot(engineAssetsRoot, EditorLayer::GetText("Engine Content"));
			}
		}

		std::vector<std::filesystem::path> breadcrumbPaths;
		breadcrumbPaths.push_back(breadcrumbRoot);
		if (mCurrentDirectory != breadcrumbRoot)
		{
			std::filesystem::path walkPath = breadcrumbRoot;
			const std::filesystem::path relativeToRoot = std::filesystem::relative(mCurrentDirectory, breadcrumbRoot);
			for (const auto& part : relativeToRoot)
			{
				walkPath /= part;
				breadcrumbPaths.push_back(walkPath);
			}
		}

		const float toolbarWidth = ImGui::GetContentRegionAvail().x;
		const float searchWidth = std::clamp(
			toolbarWidth * 0.34f,
			ImGuiWrapper::S(180.0f),
			ImGuiWrapper::S(320.0f));
		const bool bCompactToolbar = toolbarWidth < ImGuiWrapper::S(560.0f);
		const auto drawNavigateUp = [&]()
		{
			const bool bCanNavigateUp = mCurrentDirectory != breadcrumbRoot;
			if (!bCanNavigateUp)
				ImGui::BeginDisabled();
			if (ImGui::Button("<###ContentBrowserUp", ImGuiWrapper::S(26.0f, 0.0f)))
			{
				mCurrentDirectory = mCurrentDirectory.parent_path();
				mSelectedDirectory = mCurrentDirectory;
				mSelectedEntry.reset();
				RefreshDirectoryCache();
			}
			if (!bCanNavigateUp)
				ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", EditorLayer::GetText("Up one level"));
		};
		const auto drawBreadcrumbs = [&]()
		{
			for (size_t index = 0; index < breadcrumbPaths.size(); ++index)
			{
				const std::filesystem::path& breadcrumbPath = breadcrumbPaths[index];
				std::string crumbLabel = index == 0
					? breadcrumbRootLabel
					: Utils::PathToUtf8(breadcrumbPath.filename());
				ImGui::PushID(static_cast<int>(index));
				if (ImGui::SmallButton(crumbLabel.c_str()))
				{
					mSelectedDirectory = breadcrumbPath;
					mCurrentDirectory = breadcrumbPath;
					mSelectedEntry.reset();
					RefreshDirectoryCache();
				}
				ImGui::PopID();
				if (index + 1 < breadcrumbPaths.size())
				{
					ImGuiWrapper::SameLine(0.0f, 4.0f);
					ImGui::TextUnformatted(">");
					ImGuiWrapper::SameLine(0.0f, 4.0f);
				}
			}
			if (Utils::IsReadOnlyContentPath(mCurrentDirectory))
			{
				ImGuiWrapper::SameLine(0.0f);
				ImGui::TextDisabled("(%s)", EditorLayer::GetText("Read-only"));
			}
		};
		std::string_view searchText;
		const std::vector<DirectoryEntry>* contentEntries = &mCachedEntries;
		size_t visibleEntryCount = 0;
		bool bSearchViewUpdated = false;
		const auto passesSearchFilter = [&](const DirectoryEntry& entry)
		{
			return Utils::ContainsCaseInsensitive(entry.filenameString, searchText);
		};
		const auto updateSearchView = [&]()
		{
			searchText = std::string_view(mSearchBuffer);
			if (!searchText.empty() &&
				(bSearchCacheDirty || mCachedSearchDirectory != mCurrentDirectory))
			{
				HE_PROFILE_SECTION("Refresh Recursive Search Cache");
				RefreshSearchCache();
			}

			contentEntries = searchText.empty() ? &mCachedEntries : &mCachedSearchEntries;
			visibleEntryCount = static_cast<size_t>(std::count_if(
				contentEntries->begin(),
				contentEntries->end(),
				passesSearchFilter));
			bSearchViewUpdated = true;
		};
		const auto drawSearchTools = [&]()
		{
			if (ImGui::BeginTable("##ContentSearchTools", 4,
				ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadInnerX))
			{
				ImGui::TableSetupColumn("##SearchInput", ImGuiTableColumnFlags_WidthStretch, 1.0f);
				ImGui::TableSetupColumn("##ClearSearch", ImGuiTableColumnFlags_WidthFixed, ImGuiWrapper::S(30.0f));
				ImGui::TableSetupColumn("##ItemCount", ImGuiTableColumnFlags_WidthFixed, ImGuiWrapper::S(64.0f));
				ImGui::TableSetupColumn("##BrowserSettings", ImGuiTableColumnFlags_WidthFixed, ImGuiWrapper::S(30.0f));
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::InputTextWithHint(
					"##ContentSearch",
					EditorLayer::GetText("Search current folder and subfolders..."),
					mSearchBuffer,
					sizeof(mSearchBuffer),
					ImGuiInputTextFlags_EscapeClearsAll);

				ImGui::TableSetColumnIndex(1);
				const bool bHasSearchText = mSearchBuffer[0] != '\0';
				if (!bHasSearchText)
					ImGui::BeginDisabled();
				if (ImGui::Button("x###ClearContentSearch", ImGuiWrapper::S(26.0f, 0.0f)))
					mSearchBuffer[0] = '\0';
				if (!bHasSearchText)
					ImGui::EndDisabled();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					ImGui::SetTooltip("%s", EditorLayer::GetText("Clear Search"));

				ImGui::TableSetColumnIndex(2);
				updateSearchView();
				const std::string compactItemCount = searchText.empty()
					? std::to_string(visibleEntryCount)
					: std::to_string(visibleEntryCount) + "/" + std::to_string(contentEntries->size());
				const float itemCountWidth = ImGui::CalcTextSize(compactItemCount.c_str()).x;
				const float itemCountOffset = std::max(0.0f, ImGui::GetContentRegionAvail().x - itemCountWidth);
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + itemCountOffset);
				ImGui::TextDisabled("%s", compactItemCount.c_str());
				if (ImGui::IsItemHovered())
				{
					std::string itemCountTooltip = EditorLayer::GetText("Items");
					itemCountTooltip += ": " + std::to_string(visibleEntryCount);
					if (!searchText.empty())
						itemCountTooltip += " / " + std::to_string(contentEntries->size());
					ImGui::SetTooltip("%s", itemCountTooltip.c_str());
				}

				ImGui::TableSetColumnIndex(3);
				if (ImGui::ImageButton(
						"##ContentBrowserSettings",
						IconManager::ToImTextureID(IconManager::GetInstance().GetSettingIcon()),
						ImGuiWrapper::S(16.0f, 16.0f),
						ImVec2(0.0f, 0.0f),
						ImVec2(1.0f, 1.0f),
						ImVec4(0.0f, 0.0f, 0.0f, 0.0f),
						ImVec4(0.90f, 0.90f, 0.92f, 0.90f)))
					ImGui::OpenPopup("##ContentBrowserSettingsPopup");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", EditorLayer::GetText("Settings"));
				if (ImGui::BeginPopup("##ContentBrowserSettingsPopup"))
				{
					ImGui::TextDisabled("%s", EditorLayer::GetText("Thumbnail Size"));
					ImGui::SetNextItemWidth(ImGuiWrapper::S(190.0f));
					ImGui::SliderFloat("##ThumbnailSize", &mThumbnailSize, 48.0f, 256.0f, "%.0f");
					ImGui::EndPopup();
				}

				ImGui::EndTable();
			}
		};
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGuiWrapper::S(4.0f));
		const int headerColumnCount = bCompactToolbar ? 2 : 3;
		if (ImGui::BeginTable("##ContentHeader", headerColumnCount,
			ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadInnerX))
		{
			ImGui::TableSetupColumn("##Up", ImGuiTableColumnFlags_WidthFixed, ImGuiWrapper::S(34.0f));
			ImGui::TableSetupColumn("##Crumbs", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			if (!bCompactToolbar)
				ImGui::TableSetupColumn("##Search", ImGuiTableColumnFlags_WidthFixed, searchWidth);
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			drawNavigateUp();

			ImGui::TableSetColumnIndex(1);
			drawBreadcrumbs();

			if (!bCompactToolbar)
			{
				ImGui::TableSetColumnIndex(2);
				drawSearchTools();
			}

			ImGui::EndTable();
		}
		if (bCompactToolbar)
			drawSearchTools();
		ImGui::PopStyleVar();

		if (!bSearchViewUpdated)
			updateSearchView();
		const std::vector<DirectoryEntry>& displayedEntries = *contentEntries;

		ImGui::Separator();
		if (visibleEntryCount == 0)
		{
			ImGui::Dummy(ImGuiWrapper::S(0.0f, 28.0f));
			const char* emptyLabel = searchText.empty()
				? EditorLayer::GetText("This folder is empty.")
				: EditorLayer::GetText("No matching assets in this folder or its subfolders.");
			const float emptyLabelWidth = ImGui::CalcTextSize(emptyLabel).x;
			const float emptyLabelOffset = std::max(0.0f, (ImGui::GetContentRegionAvail().x - emptyLabelWidth) * 0.5f);
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + emptyLabelOffset);
			ImGui::TextDisabled("%s", emptyLabel);
			DrawRenamePopup();
			return;
		}

		const float scaledPadding = ImGuiWrapper::S(8.0f);
		const float scaledThumbnailSize = ImGuiWrapper::S(mThumbnailSize);
		const float labelGap = ImGuiWrapper::S(6.0f);
		const float columnGap = ImGuiWrapper::S(10.0f);
		const float cardWidth = scaledThumbnailSize + scaledPadding * 2.0f;
		const float cellWidth = cardWidth + columnGap;
		const float labelWidth = scaledThumbnailSize;
		const uint32_t thumbnailPixelSize = (uint32_t)std::max(1.0f, std::round(scaledThumbnailSize));

		float panelWidth = ImGui::GetContentRegionAvail().x;
		int columnCount = (int)(panelWidth / cellWidth);
		if (columnCount < 1)
			columnCount = 1;

		const ImGuiTableFlags gridFlags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_NoPadInnerX;
		if (ImGui::BeginTable("##ContentGrid", columnCount, gridFlags))
		{
			bool bLoadedImageThumbnailThisFrame = false;
			for (int columnIndex = 0; columnIndex < columnCount; ++columnIndex)
			{
				const std::string columnName = "##ContentGridColumn" + std::to_string(columnIndex);
				ImGui::TableSetupColumn(columnName.c_str(), ImGuiTableColumnFlags_WidthFixed, cellWidth);
			}

			for (const auto& entry : displayedEntries)
			{
				if (!passesSearchFilter(entry))
					continue;

				const auto& path = entry.path;
				const auto& relativePath = entry.relativePath;
				const auto& filenameString = entry.filenameString;
				const auto& extension = entry.extension;
				const ImVec2 labelSize = ImGui::CalcTextSize(filenameString.c_str(), nullptr, false, labelWidth);
				const float labelHeight = std::max(ImGui::GetTextLineHeight(), labelSize.y);
				const ImVec2 cardSize(cardWidth, scaledPadding + scaledThumbnailSize + labelGap + labelHeight + scaledPadding);

				ImGui::TableNextColumn();
				const float cellCursorX = ImGui::GetCursorPosX();
				ImGui::SetCursorPosX(cellCursorX + columnGap * 0.5f);

				ImGui::PushID(relativePath.c_str());
				if (!ImGui::IsRectVisible(cardSize))
				{
					ImGui::Dummy(cardSize);
					ImGui::PopID();
					continue;
				}

				ImGuiTextureHandle icon = entry.bIsDirectory ? IconManager::GetInstance().GetDirectoryIcon() : IconManager::GetInstance().GetFileIcon();
				bool bUseThumbnailImage = entry.bIsImage;
				if (entry.bIsImage)
				{
					IconManager& iconManager = IconManager::GetInstance();
					if (const ImGuiTextureHandle thumbnail = iconManager.TryGetThumbnail(
						entry.thumbnailCacheKey,
						thumbnailPixelSize))
					{
						icon = thumbnail;
					}
					else if (!bLoadedImageThumbnailThisFrame)
					{
						icon = iconManager.LoadOrFindThumbnail(
							entry.thumbnailCacheKey,
							thumbnailPixelSize);
						bLoadedImageThumbnailThisFrame = true;
						bUseThumbnailImage = icon != iconManager.GetFileIcon();
					}
					else
					{
						bUseThumbnailImage = false;
					}
				}
				else if (extension == "hmat" && materialThumbnails)
				{
					const std::string materialPath = Utils::NormalizeAssetPath(
						(std::filesystem::path("Assets") / relativePath).generic_string());
					materialThumbnails->Request(materialPath, mThumbnailDirectoryGeneration);
					if (const ImGuiTextureHandle materialThumbnail = materialThumbnails->TryGet(materialPath))
					{
						icon = materialThumbnail;
						bUseThumbnailImage = true;
					}
					else
					{
						icon = IconManager::GetInstance().Get("MaterialIcon");
					}
				}
				else if (extension == "he" && sceneThumbnails &&
					!Utils::IsReadOnlyContentPath(path))
				{
					const std::string scenePath = Utils::NormalizeAssetPath(
						(std::filesystem::path("Assets") / relativePath).generic_string());
					sceneThumbnails->Request(scenePath, mThumbnailDirectoryGeneration);
					if (const ImGuiTextureHandle sceneThumbnail = sceneThumbnails->TryGet(scenePath))
					{
						icon = sceneThumbnail;
						bUseThumbnailImage = true;
					}
					else
					{
						icon = IconManager::GetInstance().Get("SceneIcon");
					}
				}
				else if (Utils::iconExtToName.count(extension))
				{
					icon = IconManager::GetInstance().Get(Utils::iconExtToName.at(extension));
				}

				const ImVec2 cardMin = ImGui::GetCursorScreenPos();
				ImGui::InvisibleButton("##ContentItem", cardSize);
				if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right))
					mSelectedEntry = path;
				const bool bHovered = ImGui::IsItemHovered();
				const bool bActive = ImGui::IsItemActive();
				const bool bSelected = mSelectedEntry.has_value() && mSelectedEntry.value() == path;
				const ImVec2 cardMax(cardMin.x + cardSize.x, cardMin.y + cardSize.y);
				const ImVec2 previewMin(cardMin.x + scaledPadding, cardMin.y + scaledPadding);
				const ImVec2 previewMax(previewMin.x + scaledThumbnailSize, previewMin.y + scaledThumbnailSize);
				const float cardRounding = ImGuiWrapper::S(6.0f);
				const float previewRounding = ImGuiWrapper::S(5.0f);
				const ImU32 cardColor = ImGui::GetColorU32(
					bActive ? ImGuiCol_HeaderActive : (bSelected ? ImGuiCol_Header : (bHovered ? ImGuiCol_HeaderHovered : ImGuiCol_FrameBg)));
				const ImU32 cardBorderColor = ImGui::GetColorU32(
					bSelected || bHovered ? ImGuiCol_HeaderActive : ImGuiCol_Border);
				const ImU32 previewColor = ImGui::GetColorU32(ImGuiCol_WindowBg);
				const ImU32 previewBorderColor = ImGui::GetColorU32(ImGuiCol_Border);
				ImDrawList* drawList = ImGui::GetWindowDrawList();

				drawList->AddRectFilled(cardMin, cardMax, cardColor, cardRounding);
				drawList->AddRect(
					cardMin,
					cardMax,
					cardBorderColor,
					cardRounding,
					0,
					ImGuiWrapper::S(bSelected ? 2.0f : 1.0f));
				drawList->AddRectFilled(previewMin, previewMax, previewColor, previewRounding);

				ImVec2 imageMin = previewMin;
				ImVec2 imageMax = previewMax;
				if (!bUseThumbnailImage)
				{
					const float iconInset = scaledThumbnailSize * 0.18f;
					imageMin = ImVec2(previewMin.x + iconInset, previewMin.y + iconInset);
					imageMax = ImVec2(previewMax.x - iconInset, previewMax.y - iconInset);
				}

				const ImTextureID textureID = IconManager::ToImTextureID(icon);
				if (bUseThumbnailImage)
				{
					drawList->AddImageRounded(textureID, imageMin, imageMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32_WHITE, previewRounding);
				}
				else
				{
					drawList->AddImage(textureID, imageMin, imageMax);
				}
				drawList->AddRect(previewMin, previewMax, previewBorderColor, previewRounding, 0, ImGuiWrapper::S(1.0f));

				const ImVec2 textPos(cardMin.x + scaledPadding, previewMax.y + labelGap);
				drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), textPos, ImGui::GetColorU32(ImGuiCol_Text), filenameString.c_str(), nullptr, labelWidth);

				if (bHovered && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && ImGui::BeginTooltip())
				{
					ImGui::PushTextWrapPos(ImGuiWrapper::S(420.0f));
					ImGui::TextUnformatted(filenameString.c_str());
					ImGui::PopTextWrapPos();
					if (relativePath != filenameString)
						ImGui::TextDisabled("%s", relativePath.c_str());
					ImGui::EndTooltip();
				}

				if (ImGui::BeginPopupContextItem())
				{
					const bool bReadOnly = Utils::IsReadOnlyContentPath(path);
					ImGuiWrapper::SeparatorWithText(EditorLayer::GetText("Common"), 150.0f);
					if (bReadOnly)
						ImGui::BeginDisabled();
					if (ImGui::MenuItem(EditorLayer::GetText("Rename")))
					{
						BeginRename(entry);
					}
					if (bReadOnly)
						ImGui::EndDisabled();
					if (ImGui::MenuItem(EditorLayer::GetText("Open in Explorer")))
					{
						FileDialogs::OpenInExplorer(path.string());
					}

					ImGuiWrapper::SeparatorWithText(EditorLayer::GetText("Path"), 150.0f);
					if (ImGui::MenuItem(EditorLayer::GetText("Copy Absolute Path")))
					{
						const std::string absolutePath = Utils::PathToUtf8(path.lexically_normal());
						ImGui::SetClipboardText(absolutePath.c_str());
					}
					if (ImGui::MenuItem(EditorLayer::GetText("Copy Relative Path")))
					{
						const std::string relativeClipboardPath = Utils::NormalizeAssetPath(
							ConfigManager::GetRelativePath(path));
						ImGui::SetClipboardText(relativeClipboardPath.c_str());
					}

					ImGuiWrapper::SeparatorWithText(EditorLayer::GetText("Delete"), 150.0f);
					if (bReadOnly)
						ImGui::BeginDisabled();
					if (ImGui::MenuItem(EditorLayer::GetText("Delete"), "Del"))
					{
						try
						{
							std::error_code relativeError;
							const std::filesystem::path assetRelative = std::filesystem::relative(
								path,
								ConfigManager::GetInstance().GetAssetsFolder(),
								relativeError);
							const std::string materialPath = relativeError
								? std::string{}
								: Utils::NormalizeAssetPath(
									(std::filesystem::path("Assets") / assetRelative).generic_string());
							if (path.extension() == ".hmat" &&
								mParent->IsMaterialPanelOpen(materialPath))
							{
								ImGui::InsertNotification({
									ImGuiToastType_Warning,
									3500,
									EditorLayer::GetText("Close the material editor before deleting this asset.") });
							}
							else if (std::filesystem::is_directory(path))
							{
								std::filesystem::remove_all(path);
							}
							else
							{
								std::filesystem::remove(path);
							}
							InvalidateDirectoryCache();
						}
						catch (const std::filesystem::filesystem_error& e)
						{
							HE_CORE_ERROR(e.what());
						}
					}
					if (bReadOnly)
						ImGui::EndDisabled();

					ImGui::EndPopup();
				}

				if (ImGui::BeginDragDropSource())
				{
					std::filesystem::path relativePathFs(relativePath);
					const wchar_t* itemPath = relativePathFs.c_str();
					ImGui::SetDragDropPayload("CONTENT_BROWSER_ITEM", itemPath, (wcslen(itemPath) + 1) * sizeof(wchar_t), ImGuiCond_Once);
					ImGui::EndDragDropSource();
				}

				if (bHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				{
					if (entry.bIsDirectory)
					{
						mCurrentDirectory = path;
						mSelectedDirectory = mCurrentDirectory;
					}
					else if (extension == "hmat")
					{
						mParent->CreateMaterialPanel(Utils::NormalizeAssetPath((std::filesystem::path("Assets") / relativePath).string()));
					}
					else if (extension == "hmesh" || extension == "hanim" || extension == "hskel")
					{
						mParent->CreateAssetPreviewPanel(Utils::NormalizeAssetPath((std::filesystem::path("Assets") / relativePath).string()));
					}
					else if (extension == "hanimctrl")
					{
						if (Utils::IsReadOnlyContentPath(path))
						{
							ImGui::InsertNotification({ ImGuiToastType_Warning, 4000,
								EditorLayer::GetText("Engine content is read-only.") });
						}
						else
						{
							mParent->CreateAnimControllerPanel(Utils::NormalizeAssetPath((std::filesystem::path("Assets") / relativePath).string()));
						}
					}
					else if (extension == "hui")
					{
						if (Utils::IsReadOnlyContentPath(path))
						{
							ImGui::InsertNotification({ ImGuiToastType_Warning, 4000,
								EditorLayer::GetText("Engine content is read-only.") });
						}
						else
						{
							mParent->CreateUIPanel(Utils::NormalizeAssetPath((std::filesystem::path("Assets") / relativePath).string()));
						}
					}
					else if (extension == "efkpkg")
					{
						const std::filesystem::path effekseerToolRoot =
							ConfigManager::GetEngineFullPath("Tools/Effekseer");
						// The upstream launcher establishes the Tool working directory and waits
						// for the concrete bin process, so layout-state persistence remains valid.
						const std::filesystem::path effekseerPath =
							effekseerToolRoot / "Effekseer.exe";
						if (!std::filesystem::exists(effekseerPath))
						{
							HE_CORE_ERROR("[Effekseer] Editor tool not found: {0}", effekseerPath.string());
							ImGui::InsertNotification({ ImGuiToastType_Error, 6000,
								EditorLayer::GetText("Effekseer editor tool was not found.") });
						}
						else
						{
							const std::string projectRelativePath = ConfigManager::GetRelativePath(path);
							const std::filesystem::path previewEffectPath =
								EffekseerPackage::ExtractForPreview(projectRelativePath);
							if (!previewEffectPath.empty())
							{
								const Utils::EffekseerPreviewFormat previewFormat =
									Utils::GetEffekseerPreviewFormat(previewEffectPath);
								if (previewFormat == Utils::EffekseerPreviewFormat::RuntimeOnly)
								{
									HE_CORE_ERROR(
										"[Effekseer] Cannot edit runtime-only SKFE effect. package='{0}', preview='{1}'",
										projectRelativePath,
										previewEffectPath.string());
									ImGui::InsertNotification({ ImGuiToastType_Error, 8000,
										EditorLayer::GetText("This effect package only contains runtime data and cannot be edited.") });
								}
								else if (previewFormat == Utils::EffekseerPreviewFormat::Unreadable)
								{
									HE_CORE_ERROR("[Effekseer] Failed to inspect preview effect: {0}", previewEffectPath.string());
									ImGui::InsertNotification({ ImGuiToastType_Error, 8000,
										EditorLayer::GetText("Could not read the Effekseer preview project.") });
								}
								else
								{
									const std::filesystem::path defaultLayoutDirectory =
										effekseerToolRoot / "DefaultLayout";
									// The launcher sets its own directory as the child process working directory.
									const std::filesystem::path runtimeLayoutDirectory =
										effekseerToolRoot;
									const std::filesystem::path cacheLayoutDirectory =
										ConfigManager::GetSavedPath("Effekseer/Layout");
									const std::array<ExternalApplicationStateFile, 2> layoutFiles = {{
										{
											defaultLayoutDirectory / "Effekseer.config.Dock.xml",
											runtimeLayoutDirectory / "Effekseer.config.Dock.xml",
											cacheLayoutDirectory / "Effekseer.config.Dock.xml"
										},
										{
											defaultLayoutDirectory / "Effekseer.imgui.ini",
											runtimeLayoutDirectory / "Effekseer.imgui.ini",
											cacheLayoutDirectory / "Effekseer.imgui.ini"
										}
									}};
									if (!FileDialogs::OpenFileWithApplicationAndStateCache(
											effekseerPath,
											previewEffectPath,
											layoutFiles))
									{
										HE_CORE_ERROR("[Effekseer] Failed to launch editor: {0}", effekseerPath.string());
										ImGui::InsertNotification({ ImGuiToastType_Error, 6000,
											EditorLayer::GetText("Failed to launch Effekseer editor.") });
									}
								}
							}
							else
							{
								ImGui::InsertNotification({ ImGuiToastType_Error, 6000,
									EditorLayer::GetText("Failed to prepare Effekseer preview.") });
							}
						}
					}
					else if (Utils::iconExtToName.count(extension) && Utils::iconExtToName.at(extension) == "MusicIcon")
					{
						mParent->OpenAudioPanel("Assets\\" + relativePath);
					}
				}

				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		DrawRenamePopup();
	}

	void ContentBrowserPanel::BeginRename(const DirectoryEntry& entry)
	{
		if (Utils::IsReadOnlyContentPath(entry.path))
			return;

		mRenamePath = entry.path;
		mRenameError.clear();
		std::memset(renameBuffer, 0, sizeof(renameBuffer));
		strcpy_s(renameBuffer, sizeof(renameBuffer), entry.filenameString.c_str());
		bRenamePopupRequested = true;
		bRenameFocusRequested = true;
	}

	void ContentBrowserPanel::DrawRenamePopup()
	{
		if (mRenamePath.empty())
			return;

		const std::string popupName = std::string(EditorLayer::GetText("Rename Asset")) + "###RenameAssetPopup";
		if (bRenamePopupRequested)
		{
			ImGui::OpenPopup(popupName.c_str());
			bRenamePopupRequested = false;
		}

		ImGuiWrapper::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
		if (ImGui::BeginPopupModal(popupName.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			const std::string parentName = Utils::PathToUtf8(mRenamePath.parent_path().filename());
			ImGui::TextDisabled("%s", parentName.c_str());
			ImGui::SetNextItemWidth(ImGuiWrapper::S(480.0f));
			if (bRenameFocusRequested)
			{
				ImGui::SetKeyboardFocusHere();
				bRenameFocusRequested = false;
			}

			const bool bCommitByEnter = ImGui::InputText("##RenameAssetInput", renameBuffer, sizeof(renameBuffer), ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue);
			if (ImGui::IsItemEdited())
				mRenameError.clear();

			if (!mRenameError.empty())
				ImGui::TextColored(ImVec4(0.95f, 0.30f, 0.25f, 1.0f), "%s", mRenameError.c_str());

			const bool bCanCommit = std::strlen(renameBuffer) > 0;
			if (!bCanCommit)
				ImGui::BeginDisabled();

			const std::string renameLabel = std::string(EditorLayer::GetText("Rename")) + "###RenameAssetCommit";
			if (ImGui::Button(renameLabel.c_str(), ImGuiWrapper::S(96.0f, 0.0f)) || bCommitByEnter)
			{
				if (CommitRename())
					ImGui::CloseCurrentPopup();
			}

			if (!bCanCommit)
				ImGui::EndDisabled();

			ImGuiWrapper::SameLine(0.0f);
			const std::string cancelLabel = std::string(EditorLayer::GetText("Cancel")) + "###RenameAssetCancel";
			if (ImGui::Button(cancelLabel.c_str(), ImGuiWrapper::S(96.0f, 0.0f)))
			{
				CancelRename();
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}
	}

	bool ContentBrowserPanel::CommitRename()
	{
		if (Utils::IsReadOnlyContentPath(mRenamePath))
		{
			mRenameError = EditorLayer::GetText("Engine content is read-only.");
			return false;
		}

		const std::string newFileName = renameBuffer;
		if (newFileName.empty())
		{
			mRenameError = EditorLayer::GetText("Name cannot be empty.");
			return false;
		}

		const std::filesystem::path newPath = mRenamePath.parent_path() / newFileName;
		if (mRenamePath.extension() == ".hmat")
		{
			std::error_code relativeError;
			const std::filesystem::path assetRelative = std::filesystem::relative(
				mRenamePath,
				ConfigManager::GetInstance().GetAssetsFolder(),
				relativeError);
			const std::string materialPath = relativeError
				? std::string{}
				: Utils::NormalizeAssetPath(
					(std::filesystem::path("Assets") / assetRelative).generic_string());
			if (mParent->IsMaterialPanelOpen(materialPath))
			{
				mRenameError = EditorLayer::GetText(
					"Close the material editor before renaming this asset.");
				return false;
			}
		}
		if (newPath.lexically_normal() == mRenamePath.lexically_normal())
		{
			CancelRename();
			return true;
		}

		std::error_code ec;
		if (std::filesystem::exists(newPath, ec))
		{
			mRenameError = EditorLayer::GetText("A file or folder with that name already exists.");
			return false;
		}

		std::filesystem::rename(mRenamePath, newPath, ec);
		if (ec)
		{
			mRenameError = ec.message();
			HE_CORE_ERROR("Failed to rename {} to {}: {}", mRenamePath.string(), newPath.string(), ec.message());
			return false;
		}

		InvalidateDirectoryCache();
		CancelRename();
		return true;
	}

	void ContentBrowserPanel::CancelRename()
	{
		mRenamePath.clear();
		mRenameError.clear();
		std::memset(renameBuffer, 0, sizeof(renameBuffer));
		bRenamePopupRequested = false;
		bRenameFocusRequested = false;
	}

	bool ContentBrowserPanel::TryBuildDirectoryEntry(
		const std::filesystem::directory_entry& filesystemEntry,
		DirectoryEntry& entry) const
	{
		std::error_code entryError;
		const bool bIsDirectory = filesystemEntry.is_directory(entryError);
		if (entryError)
			return false;

		if (!bIsDirectory && !filesystemEntry.is_regular_file(entryError))
			return false;
		if (entryError)
			return false;

		try
		{
			entry.path = filesystemEntry.path();
			entry.bIsDirectory = bIsDirectory;
			entry.relativePath = Utils::MakeRelativeBrowserPath(entry.path);
			entry.filenameString = Utils::PathToUtf8(entry.path.filename());

			entry.extension = Utils::PathToUtf8(entry.path.extension());
			if (!entry.extension.empty() && entry.extension.front() == '.')
				entry.extension.erase(entry.extension.begin());
			std::transform(entry.extension.begin(), entry.extension.end(), entry.extension.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			entry.bIsImage = Utils::IsImageFormat(entry.extension);
			if (entry.bIsImage)
				entry.thumbnailCacheKey = Utils::PathToUtf8(entry.path);
		}
		catch (const std::filesystem::filesystem_error&)
		{
			return false;
		}

		return true;
	}

	void ContentBrowserPanel::RefreshDirectoryCache()
	{
		HE_PROFILE;

		const bool bDirectoryChanged = mCachedDirectory != mCurrentDirectory;
		mCachedEntries.clear();
		std::vector<DirectoryEntry> directoryEntries;
		std::vector<DirectoryEntry> fileEntries;

		std::error_code iteratorError;
		std::filesystem::directory_iterator iterator(
			mCurrentDirectory,
			std::filesystem::directory_options::skip_permission_denied,
			iteratorError);
		const std::filesystem::directory_iterator endIterator;
		while (iterator != endIterator)
		{
			DirectoryEntry entry;
			if (TryBuildDirectoryEntry(*iterator, entry))
			{
				if (entry.bIsDirectory)
					directoryEntries.push_back(std::move(entry));
				else
					fileEntries.push_back(std::move(entry));
			}

			iterator.increment(iteratorError);
			if (iteratorError)
				break;
		}

		auto sortEntries = [](std::vector<DirectoryEntry>& entries)
			{
				std::sort(entries.begin(), entries.end(), [](const DirectoryEntry& lhs, const DirectoryEntry& rhs)
					{
						return lhs.filenameString < rhs.filenameString;
					});
			};

		sortEntries(directoryEntries);
		sortEntries(fileEntries);

		mCachedEntries.reserve(directoryEntries.size() + fileEntries.size());
		for (DirectoryEntry& entry : directoryEntries)
			mCachedEntries.push_back(std::move(entry));
		for (DirectoryEntry& entry : fileEntries)
			mCachedEntries.push_back(std::move(entry));
		std::error_code selectedEntryError;
		const bool bSelectedEntryExists = !mSelectedEntry.has_value() ||
			std::filesystem::exists(mSelectedEntry.value(), selectedEntryError);
		if (bDirectoryChanged || selectedEntryError || !bSelectedEntryExists)
		{
			mSelectedEntry.reset();
		}

		mCachedDirectory = mCurrentDirectory;
		bSearchCacheDirty = true;
		++mThumbnailDirectoryGeneration;
		if (mThumbnailDirectoryGeneration == 0)
			mThumbnailDirectoryGeneration = 1;
		bDirCacheDirty = false;
	}

	void ContentBrowserPanel::RefreshSearchCache()
	{
		HE_PROFILE;

		mCachedSearchEntries.clear();
		std::error_code iteratorError;
		std::filesystem::recursive_directory_iterator iterator(
			mCurrentDirectory,
			std::filesystem::directory_options::skip_permission_denied,
			iteratorError);
		const std::filesystem::recursive_directory_iterator endIterator;
		while (iterator != endIterator)
		{
			DirectoryEntry entry;
			if (TryBuildDirectoryEntry(*iterator, entry) && !entry.bIsDirectory)
				mCachedSearchEntries.push_back(std::move(entry));

			iterator.increment(iteratorError);
			if (iteratorError)
				break;
		}

		std::sort(
			mCachedSearchEntries.begin(),
			mCachedSearchEntries.end(),
			[](const DirectoryEntry& lhs, const DirectoryEntry& rhs)
			{
				return lhs.relativePath < rhs.relativePath;
			});

		mCachedSearchDirectory = mCurrentDirectory;
		bSearchCacheDirty = false;
	}

	bool ContentBrowserPanel::OnKeyPressedTree(KeyPressedEvent& e)
	{
		// Shortcuts
		if (e.GetRepeatCount() > 0)
			return false;

		switch (e.GetKeyCode())
		{
			case Key::Delete:
			{
				HE_CORE_INFO("Tree Delete!");
				return true;
			}
			default:
			{
				return false;
			}
		}
		return false;
	}

	bool ContentBrowserPanel::OnKeyPressedContent(KeyPressedEvent& e)
	{
		// Shortcuts
		if (e.GetRepeatCount() > 0)
			return false;

		switch (e.GetKeyCode())
		{
		case Key::Delete:
		{
			HE_CORE_INFO("Content Delete!");
			return true;
		}
		default:
		{
			return false;
		}
		}
		return false;
	}

	void ContentBrowserPanel::NewFolder()
	{
		if (Utils::IsReadOnlyContentPath(mCurrentDirectory))
			return;

		auto newFolder = AssetManager::GetAvailableFileName("EmptyFolder", mCurrentDirectory);
		std::filesystem::create_directory(newFolder);
		InvalidateDirectoryCache();
	}
}
