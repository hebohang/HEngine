#include "ContentBrowserPanel.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"

#include <imgui/imgui.h>

namespace HEngine
{
	namespace Utils
	{
		static bool HaveDirectoryMember(std::filesystem::path currentPath)
		{
			for (auto& directoryEntry : std::filesystem::directory_iterator(currentPath))
			{
				if (directoryEntry.is_directory())
					return true;
			}
			return false;
		}
	}

	ContentBrowserPanel::ContentBrowserPanel()
		: mCurrentDirectory(ConfigManager::GetInstance().GetAssetsFolder())
	{
		mDirectoryIcon = Texture2D::Create(AssetManager::GetInstance().GetFullPath("Resources/Icons/ContentBrowser/DirectoryIcon.png").string());
		mFileIcon = Texture2D::Create(AssetManager::GetInstance().GetFullPath("Resources/Icons/ContentBrowser/FileIcon.png").string());
	}

	void ContentBrowserPanel::OnImGuiRender(bool* pOpen)
	{
		// from imgui_demo.cpp: ShowAboutWindow function, should we add flag ImGuiWindowFlags_AlwaysAutoResize?
		if (!ImGui::Begin("Content Browser", pOpen))
		{
			ImGui::End();
			return;
		}

		ImGui::Columns(2);

		if (ImGui::BeginChild("CONTENT_BROWSER_TREE"))
		{
			DrawTree();
		}
		ImGui::EndChild();

		ImGui::NextColumn();

		if (ImGui::BeginChild("CONTENT_BROWSER_CONTENT"))
		{
			DrawContent();
		}
		ImGui::EndChild();

		ImGui::Columns(1);

		ImGui::End();
	}

	void ContentBrowserPanel::DrawTree()
	{
		DrawTreeRecursive(ConfigManager::GetInstance().GetAssetsFolder());
	}

	void ContentBrowserPanel::DrawTreeRecursive(const std::filesystem::path& currentPath)
	{
		const ImGuiTreeNodeFlags baseFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
			ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_SpanFullWidth;

		ImGuiTreeNodeFlags nodeFlags = baseFlags;

		if (mSelectedDirectory && *mSelectedDirectory == currentPath)
		{
			nodeFlags |= ImGuiTreeNodeFlags_Selected;
		}

		bool bNeedOpen = true;
		if (!Utils::HaveDirectoryMember(currentPath))
		{
			nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			bNeedOpen = false;
		}

		//std::string label = "##" + currentPath.filename().string();
		bool nodeOpen = ImGui::TreeNodeEx(currentPath.filename().string().c_str(), nodeFlags);
		//ImGui::SameLine();
		//ImGui::ImageButton((ImTextureID)mDirectoryIcon->GetRendererID(), { 20.0f, 20.0f }, { 0, 1 }, { 1, 0 });
		//ImGui::SameLine();
		//ImGui::Text(currentPath.filename().string().c_str());

		if (ImGui::IsItemClicked())
		{
			mSelectedDirectory = currentPath;
		}

		if (nodeOpen && bNeedOpen)
		{
			for (auto& p : std::filesystem::directory_iterator(currentPath))
			{
				const auto& path = p.path();
				if (!std::filesystem::is_directory(path))
				{
					continue;
				}

				DrawTreeRecursive(path);
			}
			ImGui::TreePop();
		}
	}

	void ContentBrowserPanel::DrawContent()
	{
		if (!mSelectedDirectory)
		{
			return;
		}

		mCurrentDirectory = *mSelectedDirectory;

		static float padding = 16.0f;
		static float thumbnailSize = 128.0f;
		float cellSize = thumbnailSize + padding;

		float panelWidth = ImGui::GetContentRegionAvail().x;
		int columnCount = (int)(panelWidth / cellSize);
		if (columnCount < 1)
			columnCount = 1;

		ImGui::Columns(columnCount, 0, false);

		for (auto& directoryEntry : std::filesystem::directory_iterator(mCurrentDirectory))
		{
			const auto& path = directoryEntry.path();
			auto relativePath = std::filesystem::relative(path, ConfigManager::GetInstance().GetAssetsFolder());
			std::string filenameString = relativePath.filename().string();

			ImGui::PushID(filenameString.c_str());
			Ref<Texture2D> icon = directoryEntry.is_directory() ? mDirectoryIcon : mFileIcon;
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
			ImGui::ImageButton((ImTextureID)icon->GetRendererID(), { thumbnailSize, thumbnailSize }, { 0, 1 }, { 1, 0 });

			if (ImGui::BeginDragDropSource())
			{
				const wchar_t* itemPath = relativePath.c_str();
				ImGui::SetDragDropPayload("CONTENT_BROWSER_ITEM", itemPath, (wcslen(itemPath) + 1) * sizeof(wchar_t), ImGuiCond_Once);
				ImGui::EndDragDropSource();
			}

			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				if (directoryEntry.is_directory())
				{
					mCurrentDirectory /= path.filename();
					mSelectedDirectory = mCurrentDirectory;
				}
			}
			ImGui::TextWrapped(filenameString.c_str());

			ImGui::NextColumn();

			ImGui::PopID();
		}

		ImGui::Columns(1);

		ImGui::SliderFloat("Thumbnail Size", &thumbnailSize, 16, 512);
		ImGui::SliderFloat("Padding", &padding, 0, 32);
	}
}
