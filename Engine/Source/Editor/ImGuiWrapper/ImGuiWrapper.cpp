#include "Editor/ImGuiWrapper/ImGuiWrapper.h"

#include "Runtime/Core/AppFramework/Application.h"

#include <string>

namespace HEngine::ImGuiWrapper
{
    float GetUIScale()
    {
        ImGuiLayer* imguiLayer = Application::GetInstance().GetImGuiLayer();
        return imguiLayer ? imguiLayer->GetUIScale() : 1.0f;
    }

    float S(float value)
    {
        return value * GetUIScale();
    }

    ImVec2 S(const ImVec2& value)
    {
        const float uiScale = GetUIScale();
        return ImVec2(value.x * uiScale, value.y * uiScale);
    }

    ImVec2 S(float x, float y)
    {
        const float uiScale = GetUIScale();
        return ImVec2(x * uiScale, y * uiScale);
    }

    void SetColumnWidth(int columnIndex, float width)
    {
        ImGui::SetColumnWidth(columnIndex, S(width));
    }

    void SetNextWindowSize(const ImVec2& size, ImGuiCond cond)
    {
        ImGui::SetNextWindowSize(S(size), cond);
    }

    void SetNextWindowSizeConstraints(const ImVec2& minSize, const ImVec2& maxSize)
    {
        ImGui::SetNextWindowSizeConstraints(S(minSize), S(maxSize));
    }

    void ConstrainNextWindowToWorkArea(const char* name, const ImVec2& defaultSize, const ImVec2& minSize)
    {
        ImGuiWindow* window = ImGui::FindWindowByName(name);
        if (window && window->DockId != 0)
            return;
        const ImGuiViewport* viewport = window && window->Viewport
            ? window->Viewport : ImGui::GetMainViewport();
        // WorkPos/WorkSize are already pixels. Only design dimensions use S().
        const ImVec2 margin = S(12.0f, 12.0f);
        const ImVec2 inset(ImMin(margin.x, viewport->WorkSize.x * 0.25f),
            ImMin(margin.y, viewport->WorkSize.y * 0.25f));
        const ImVec2 origin(viewport->WorkPos.x + inset.x, viewport->WorkPos.y + inset.y);
        const ImVec2 maximum(ImMax(1.0f, viewport->WorkSize.x - inset.x * 2.0f),
            ImMax(1.0f, viewport->WorkSize.y - inset.y * 2.0f));
        const ImVec2 scaledMin = S(minSize);
        const ImVec2 minimum(ImMin(scaledMin.x, maximum.x), ImMin(scaledMin.y, maximum.y));
        ImGui::SetNextWindowSizeConstraints(minimum, maximum);
        const ImVec2 desired = window ? window->SizeFull : S(defaultSize);
        const ImVec2 size(ImClamp(desired.x, minimum.x, maximum.x),
            ImClamp(desired.y, minimum.y, maximum.y));
        if (!window || size.x != desired.x || size.y != desired.y)
            ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        const ImVec2 desiredPos = window ? window->Pos : origin;
        const ImVec2 position(ImClamp(desiredPos.x, origin.x, origin.x + maximum.x - size.x),
            ImClamp(desiredPos.y, origin.y, origin.y + maximum.y - size.y));
        if (!window || position.x != desiredPos.x || position.y != desiredPos.y)
            ImGui::SetNextWindowPos(position, ImGuiCond_Always);
    }

    void SameLine(float offsetFromStartX, float spacing)
    {
        if (spacing >= 0.0f)
            ImGui::SameLine(S(offsetFromStartX), S(spacing));
        else
            ImGui::SameLine(S(offsetFromStartX));
    }

    bool HasScaleChanged(float& lastScale, float epsilon)
    {
        const float currentScale = GetUIScale();
        if (std::abs(lastScale - currentScale) > epsilon)
        {
            lastScale = currentScale;
            return true;
        }
        return false;
    }

    bool BeginResizableSplit(const char* id, float leftWeight, float rightWeight, ImGuiTableFlags extraFlags)
    {
        const ImGuiTableFlags flags = ImGuiTableFlags_Resizable |
            ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_SizingStretchProp |
            extraFlags;

        if (!ImGui::BeginTable(id, 2, flags))
            return false;

        ImGui::TableSetupColumn("##SplitLeft", ImGuiTableColumnFlags_WidthStretch, leftWeight);
        ImGui::TableSetupColumn("##SplitRight", ImGuiTableColumnFlags_WidthStretch, rightWeight);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        return true;
    }

    void EndResizableSplit()
    {
        ImGui::EndTable();
    }

    bool InputTextLeft(const char* label, char* buf, size_t buf_size, ImGuiInputTextFlags flags, ImGuiInputTextCallback callback, void* user_data)
    {
        ImGui::Text(label);
        ImGui::SameLine();
        std::string InputLabel = std::string("##") + std::string(label);
        return ImGui::InputText(InputLabel.c_str(), buf, buf_size, flags, callback, user_data);
    }

    bool TreeNodeExStyle1(const void* str_id, const std::string& name, ImGuiTreeNodeFlags flags)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, S(3.0f, 2.0f));
        ImGui::Separator();
        bool open = ImGui::TreeNodeEx((void*)str_id, flags, name.c_str());
        ImGui::PopStyleVar();
        return open;
    }

    // SubNode
    bool TreeNodeExStyle2(const void* str_id, const std::string& name, ImGuiTreeNodeFlags flags)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, S(1.0f, 1.0f));
        bool open = ImGui::TreeNodeEx((void*)str_id, flags, name.c_str());
        ImGui::PopStyleVar();
        return open;
    }

    void CenteredSeparator(float width)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return;
        ImGuiContext& g = *GImGui;

        // Horizontal Separator
        float x1, x2;
        if (window->DC.CurrentColumns == NULL && (width == 0))
        {
            // Span whole window
            ///x1 = window->Pos.x; // This fails with SameLine(); CenteredSeparator();
            // Nah, we have to detect if we have a sameline in a different way
            x1 = window->DC.CursorPos.x;
            x2 = x1 + window->Size.x;
        }
        else
        {
            // Start at the cursor
            x1 = window->DC.CursorPos.x;
            if (width != 0) {
                x2 = x1 + width;
            }
            else
            {
                x2 = window->ClipRect.Max.x;
                // Pad right side of columns (except the last one)
                if (window->DC.CurrentColumns && (window->DC.CurrentColumns->Current < window->DC.CurrentColumns->Count - 1))
                    x2 -= g.Style.ItemSpacing.x;
            }
        }
        float y1 = window->DC.CursorPos.y + int(window->DC.CurrLineSize.y / 2.0f);
        float y2 = y1 + S(1.0f);

        window->DC.CursorPos.x += width; //+ g.Style.ItemSpacing.x;

        if (!window->DC.ItemWidthStack.empty())
            x1 += window->DC.Indent.x;

        const ImRect bb(ImVec2(x1, y1), ImVec2(x2, y2));
        ImGui::ItemSize(ImVec2(0.0f, 0.0f)); // NB: we don't provide our width so that it doesn't get feed back into AutoFit, we don't provide height to not alter layout.
        if (!ImGui::ItemAdd(bb, NULL))
        {
            return;
        }

        window->DrawList->AddLine(bb.Min, ImVec2(bb.Max.x, bb.Min.y), ImGui::GetColorU32(ImGuiCol_Border));
    }

    void SeparatorWithText(const char* text, float lineSize, float separatorTextGray)
    {
        ImGui::TextColored(ImColor(separatorTextGray, separatorTextGray, separatorTextGray, 1.0f), text);
        ImGui::SameLine();
        ImGuiWrapper::CenteredSeparator(S(lineSize) - ImGui::CalcTextSize(text).x);
    }
}
