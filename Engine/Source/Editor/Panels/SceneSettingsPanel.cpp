#include "Editor/Panels/SceneSettingsPanel.h"
#include "Editor/IconManager/IconManager.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Renderer/Renderer3D.h"

#include <imgui/imgui.h>

#include <regex>

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
        mFront  = mDefaultTexture;
        mBack   = mDefaultTexture;
    }

	// Still some bugs (top bottom inverse?), see https://stackoverflow.com/questions/55558241/opengl-cubemap-face-order-sampling-issue
    void SceneSettingsPanel::OnImGuiRender(bool* pOpen)
    {
        if (!ImGui::Begin("Scene Settings", pOpen))
        {
            ImGui::End();
            return;
        }

		const auto& SkyBoxTreeNode = [&mPaths = mPaths](const char* nodeName, Ref<Texture>& tex, uint32_t pathIndex) {
			if (ImGui::TreeNode(nodeName))
			{
				ImGui::Image((ImTextureID)tex->GetRendererID(), ImVec2(64, 64), ImVec2{ 0, 1 }, ImVec2{ 1, 0 });
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
					{
						auto path = (const wchar_t*)payload->Data;
						std::string relativePath = (std::filesystem::path("Assets") / path).string();
						std::filesystem::path texturePath = ConfigManager::GetInstance().GetAssetsFolder() / path;
						mPaths[pathIndex] = texturePath.string();
						relativePath = std::regex_replace(relativePath, std::regex("\\\\"), "/");
						tex = IconManager::GetInstance().LoadOrFindTexture(relativePath);
						Renderer3D::GetSkyBox()->SetFace((FaceTarget)pathIndex, relativePath);
					}
					ImGui::EndDragDropTarget();
				}
				ImGui::TreePop();
			}
		};

		//Sky Box
		if (ImGui::TreeNode("SkyBox"))
		{
			if (ImGui::Button("Update"))
			{
				Renderer3D::GetSkyBox()->Generate();
			}

			SkyBoxTreeNode("Right +X",  mRight,  0);
			SkyBoxTreeNode("Left -X",   mLeft,   1);
			SkyBoxTreeNode("Top +Y",    mTop,    2);
			SkyBoxTreeNode("Bottom -Y", mBottom, 3);
			SkyBoxTreeNode("Front +Z",  mFront,  4);
			SkyBoxTreeNode("Back -Z",   mBack,   5);

			ImGui::TreePop();
		}

        ImGui::End();
    }
}