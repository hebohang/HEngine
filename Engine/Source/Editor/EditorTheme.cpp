#include "EditorTheme.h"

#include "Editor/ImGuiWrapper/ImGuiWrapper.h"

namespace HEngine::EditorTheme
{
    void DrawPill(const char* text, const ImVec4& backgroundColor, const ImVec4& textColor)
    {
        if (!text || text[0] == '\0')
            return;

        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (!window || window->SkipItems)
            return;

        const ImVec2 textSize = ImGui::CalcTextSize(text);
        const ImVec2 padding = ImGuiWrapper::S(10.0f, 3.0f);
        const ImVec2 size(textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
        const float rounding = ImGuiWrapper::S(999.0f);
        const ImVec4 borderColor(
            ImMin(backgroundColor.x + 0.08f, 1.0f),
            ImMin(backgroundColor.y + 0.08f, 1.0f),
            ImMin(backgroundColor.z + 0.08f, 1.0f),
            ImMin(backgroundColor.w + 0.10f, 1.0f));

        ImGui::ItemSize(size);
        if (!ImGui::ItemAdd(bb, 0))
            return;

        window->DrawList->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(backgroundColor), rounding);
        window->DrawList->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(borderColor), rounding, 0, ImGuiWrapper::S(1.0f));
        window->DrawList->AddText(ImVec2(bb.Min.x + padding.x, bb.Min.y + padding.y), ImGui::GetColorU32(textColor), text);
    }

    void DrawSectionHeader(const char* title, const char* subtitle, const ImVec4& accentColor)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (!window || window->SkipItems)
            return;

        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float accentWidth = ImGuiWrapper::S(3.0f);
        const float accentHeight = subtitle && subtitle[0] != '\0'
            ? ImGuiWrapper::S(28.0f)
            : ImGuiWrapper::S(18.0f);

        window->DrawList->AddRectFilled(
            start,
            ImVec2(start.x + accentWidth, start.y + accentHeight),
            ImGui::GetColorU32(accentColor),
            ImGuiWrapper::S(2.0f));

        ImGui::SetCursorScreenPos(ImVec2(start.x + ImGuiWrapper::S(10.0f), start.y - ImGuiWrapper::S(1.0f)));
        ImGui::TextUnformatted(title ? title : "Section");
        if (subtitle && subtitle[0] != '\0')
            ImGui::TextDisabled("%s", subtitle);
    }
}
