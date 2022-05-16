#include "Editor/ImGuiWrapper/ImGuiWrapper.h"

#include <string>

namespace HEngine::ImGuiWrapper
{
    bool InputTextLeft(const char* label, char* buf, size_t buf_size, ImGuiInputTextFlags flags, ImGuiInputTextCallback callback, void* user_data)
    {
        ImGui::Text(label);
        ImGui::SameLine();
        std::string InputLabel = std::string("##") + std::string(label);
        return ImGui::InputText(InputLabel.c_str(), buf, buf_size, flags, callback, user_data);
    }

    bool TreeNodeExStyle1(const void* str_id, const std::string& name, ImGuiTreeNodeFlags flags)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{ 4.0f, 4.0f });
        ImGui::Separator();
        bool open = ImGui::TreeNodeEx((void*)str_id, flags, name.c_str());
        ImGui::PopStyleVar();
        return open;
    }

    // SubNode
    bool TreeNodeExStyle2(const void* str_id, const std::string& name, ImGuiTreeNodeFlags flags)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{ 2.0f, 2.0f });
        bool open = ImGui::TreeNodeEx((void*)str_id, flags, name.c_str());
        ImGui::PopStyleVar();
        return open;
    }
}