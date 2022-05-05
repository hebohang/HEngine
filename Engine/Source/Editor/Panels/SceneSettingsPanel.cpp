#include "Editor/Panels/SceneSettingsPanel.h"
#include "Editor/IconManager/IconManager.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Renderer/Renderer3D.h"

#include <imgui/imgui.h>

namespace HEngine 
{
    SceneSettingsPanel::SceneSettingsPanel()
    {
		Ref<Texture> mDefaultTexture = IconManager::GetInstance().GetNullTexture();

        mPaths = std::vector<std::string>(6);
        mRight  = mDefaultTexture;
        mLeft   = mDefaultTexture;
        mTop    = mDefaultTexture;
        mBottom = mDefaultTexture;
        mBack   = mDefaultTexture;
        mFront  = mDefaultTexture;
    }

    void SceneSettingsPanel::OnImGuiRender(bool* pOpen)
    {
        if (!ImGui::Begin("Scene Settings", pOpen))
        {
            ImGui::End();
            return;
        }

		//Sky Box
		if (ImGui::TreeNode("SkyBox"))
		{
			ImGui::Text("Right +X");
			ImGui::Image((ImTextureID)mRight->GetRendererID(), ImVec2(64, 64));
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
				{
					const wchar_t* path = (const wchar_t*)payload->Data;
					std::filesystem::path texturePath = ConfigManager::GetInstance().GetAssetsFolder() / path;
					mPaths[0] = texturePath.string();
					mRight = Texture2D::Create(texturePath.string());
					Renderer3D::GetSkyBox()->SetFace(FaceTarget::Right, texturePath.string());
				}
				ImGui::EndDragDropTarget();
			}
			ImGui::Text("Left -X");
			ImGui::Image((ImTextureID)mLeft->GetRendererID(), ImVec2(64, 64));
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
				{
					const wchar_t* path = (const wchar_t*)payload->Data;
					std::filesystem::path texturePath = ConfigManager::GetInstance().GetAssetsFolder() / path;
					mPaths[1] = texturePath.string();
					mLeft = Texture2D::Create(texturePath.string());
					Renderer3D::GetSkyBox()->SetFace(FaceTarget::Left, texturePath.string());
				}
				ImGui::EndDragDropTarget();
			}
			ImGui::Text("Top +Y");
			ImGui::Image((ImTextureID)mTop->GetRendererID(), ImVec2(64, 64));
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
				{
					const wchar_t* path = (const wchar_t*)payload->Data;
					std::filesystem::path texturePath = ConfigManager::GetInstance().GetAssetsFolder() / path;
					mPaths[2] = texturePath.string();
					mTop = Texture2D::Create(texturePath.string());
					Renderer3D::GetSkyBox()->SetFace(FaceTarget::Top, texturePath.string());
				}
				ImGui::EndDragDropTarget();
			}
			ImGui::Text("Bottom -Y");
			ImGui::Image((ImTextureID)mBottom->GetRendererID(), ImVec2(64, 64));
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
				{
					const wchar_t* path = (const wchar_t*)payload->Data;
					std::filesystem::path texturePath = ConfigManager::GetInstance().GetAssetsFolder() / path;
					mPaths[3] = texturePath.string();
					mBottom = Texture2D::Create(texturePath.string());
					Renderer3D::GetSkyBox()->SetFace(FaceTarget::Bottom, texturePath.string());
				}
				ImGui::EndDragDropTarget();
			}
			ImGui::Text("Back +Z");
			ImGui::Image((ImTextureID)mBack->GetRendererID(), ImVec2(64, 64));
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
				{
					const wchar_t* path = (const wchar_t*)payload->Data;
					std::filesystem::path texturePath = ConfigManager::GetInstance().GetAssetsFolder() / path;
					mPaths[4] = texturePath.string();
					mBack = Texture2D::Create(texturePath.string());
					Renderer3D::GetSkyBox()->SetFace(FaceTarget::Back, texturePath.string());
				}
				ImGui::EndDragDropTarget();
			}
			ImGui::Text("Front -Z");
			ImGui::Image((ImTextureID)mFront->GetRendererID(), ImVec2(64, 64));
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
				{
					const wchar_t* path = (const wchar_t*)payload->Data;
					std::filesystem::path texturePath = ConfigManager::GetInstance().GetAssetsFolder() / path;
					mPaths[5] = texturePath.string();
					mFront = Texture2D::Create(texturePath.string());
					Renderer3D::GetSkyBox()->SetFace(FaceTarget::Front, texturePath.string());
				}
				ImGui::EndDragDropTarget();
			}

			ImGui::TreePop();
		}

        ImGui::End();
    }
}