#include "ContentBrowserPanel.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"

#include <imgui/imgui.h>

namespace HEngine
{
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

		}
		ImGui::EndChild();

		ImGui::NextColumn();
		ImGui::Separator();

		ImGui::BeginChild("CONTENT_BROWSER_CONTENT");

		if (mCurrentDirectory != std::filesystem::path(ConfigManager::GetInstance().GetAssetsFolder()))
		{
			if (ImGui::Button("<-"))
			{
				mCurrentDirectory = mCurrentDirectory.parent_path();
			}
		}

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
					mCurrentDirectory /= path.filename();

			}
			ImGui::TextWrapped(filenameString.c_str());

			ImGui::NextColumn();

			ImGui::PopID();
		}

		ImGui::Columns(1);

		ImGui::SliderFloat("Thumbnail Size", &thumbnailSize, 16, 512);
		ImGui::SliderFloat("Padding", &padding, 0, 32);

		ImGui::EndChild();

		ImGui::Columns(1);

		// TODO: status bar
		ImGui::End();
	}
}
