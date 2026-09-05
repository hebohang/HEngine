#include "BasePanel.h"
#include "../EditorLayer.h"
#include "Editor/ImGuiWrapper/ImGuiWrapper.h"

#include <imgui/imgui.h>

#include <cfloat>

namespace HEngine 
{
    BasePanel::BasePanel(EditorLayer* parent, std::string panelName, bool* pOpen)
        : mParent(parent), mName(panelName), pOpen(pOpen)
    {
    }

    void BasePanel::OnImGuiRender()
    {
        if (!(*pOpen))
            return;

        ImGuiWrapper::SetNextWindowSizeConstraints(ImVec2(300.0f, 240.0f), ImVec2(FLT_MAX, FLT_MAX));

        // from imgui_demo.cpp: ShowAboutWindow function, should we add flag ImGuiWindowFlags_AlwaysAutoResize?
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
}
