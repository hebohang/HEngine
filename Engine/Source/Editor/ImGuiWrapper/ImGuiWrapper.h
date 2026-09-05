#pragma once

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

namespace HEngine
{
    enum class WindowCorner
    {
        LeftTop = 0,
        RightTop,
        LeftDown,
        RightDown
    };
}

namespace HEngine::ImGuiWrapper
{
    constexpr float kDefaultPropertyLabelWidth = 148.0f;
    constexpr float kWidePropertyLabelWidth = 176.0f;

    float GetUIScale();
    float S(float value);
    ImVec2 S(const ImVec2& value);
    ImVec2 S(float x, float y);

    void SetColumnWidth(int columnIndex, float width);
    void SetNextWindowSize(const ImVec2& size, ImGuiCond cond = 0);
    void SetNextWindowSizeConstraints(const ImVec2& minSize, const ImVec2& maxSize);
    void SameLine(float offsetFromStartX, float spacing = -1.0f);
    bool HasScaleChanged(float& lastScale, float epsilon = 0.01f);

    bool BeginResizableSplit(const char* id, float leftWeight = 1.8f, float rightWeight = 1.0f, ImGuiTableFlags extraFlags = 0);
    void EndResizableSplit();

    bool InputTextLeft(const char* label, char* buf, size_t buf_size, ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = NULL, void* user_data = NULL);
    bool TreeNodeExStyle1(const void* str_id, const std::string& name, ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_FramePadding);
    bool TreeNodeExStyle2(const void* str_id, const std::string& name, ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_FramePadding);

    // from https://github.com/ocornut/imgui/issues/1643
    void CenteredSeparator(float width = 0.0);
    void SeparatorWithText(const char* text, float lineSize = 300.0f, float separatorTextGray = 0.5f);

    template<typename UIFunc1, typename UIFunc2>
    void DrawTwoUI(UIFunc1 func1, UIFunc2 func2, float ColumnWidth = kDefaultPropertyLabelWidth)
    {
        const float availWidth = ImGui::GetContentRegionAvail().x;
        const float desiredWidth = ImGuiWrapper::S(ColumnWidth);
        const float minRightColumn = ImMax(ImGui::GetFrameHeight() + ImGuiWrapper::S(10.0f), ImGuiWrapper::S(44.0f));
        const float maxLeftByAvail = ImMax(0.0f, availWidth - minRightColumn);
        const float finalLeftWidth = ImMin(desiredWidth, maxLeftByAvail);

		ImGui::PushID("DrawTwoUI_1");
		if (ImGui::BeginTable("##DrawTwoUI", 2, ImGuiTableFlags_NoPadInnerX | ImGuiTableFlags_SizingFixedFit))
		{
			ImGui::TableSetupColumn("##Left", ImGuiTableColumnFlags_WidthFixed, finalLeftWidth);
			ImGui::TableSetupColumn("##Right", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			func1();

			ImGui::TableSetColumnIndex(1);
			ImGui::AlignTextToFramePadding();
			ImGui::SetNextItemWidth(-FLT_MIN);
			func2();

            ImGui::EndTable();
        }
        ImGui::PopID();
    }

    template<typename UIFunc>
    void PropertyRow(const char* label, UIFunc uiFunc, float labelWidth = kDefaultPropertyLabelWidth)
    {
        DrawTwoUI(
            [label]() { ImGui::TextUnformatted(label); },
            uiFunc,
            labelWidth);
    }

    template<typename UIFunc>
    void PropertyRowWide(const char* label, UIFunc uiFunc)
    {
        PropertyRow(label, uiFunc, kWidePropertyLabelWidth);
    }

    template<typename UIFunc1, typename UIFunc2>
    void DrawTwoUI(UIFunc1 func1, UIFunc2 func2, float Column1Width, float Column2Width)
    {
        const float availWidth = ImGui::GetContentRegionAvail().x;
        const float desiredLeftWidth = ImGuiWrapper::S(Column1Width);
        const float desiredRightWidth = ImGuiWrapper::S(Column2Width);
        const float minRightWidth = ImMax(ImGui::GetFrameHeight() + ImGuiWrapper::S(10.0f), ImMin(desiredRightWidth, ImGuiWrapper::S(220.0f)));
        const float maxLeftByAvail = ImMax(0.0f, availWidth - minRightWidth);
        const float finalLeftWidth = ImMin(desiredLeftWidth, maxLeftByAvail);

		ImGui::PushID("DrawTwoUI_2");
		if (ImGui::BeginTable("##DrawTwoUI", 2, ImGuiTableFlags_NoPadInnerX | ImGuiTableFlags_SizingFixedFit))
		{
            ImGui::TableSetupColumn("##Left", ImGuiTableColumnFlags_WidthFixed, finalLeftWidth);
            ImGui::TableSetupColumn("##Right", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            func1();

			ImGui::TableSetColumnIndex(1);
			ImGui::AlignTextToFramePadding();
			ImGui::SetNextItemWidth(-FLT_MIN);
			func2();

            ImGui::EndTable();
        }
        ImGui::PopID();
    }

    template<typename UIFunction>
    static void InternalDraw(const char* name, bool* p_open, const float distance, WindowCorner corner, UIFunction uiFunc)
    {
        ImGuiDockNode* node = ImGui::GetWindowDockNode();

        ImGuiViewport* viewport = ImGui::GetWindowViewport();
        const ImVec2 anchorPos = node ? node->Pos : ImGui::GetWindowPos();
        const ImVec2 anchorSize = node ? node->Size : ImGui::GetWindowSize();

        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
        windowFlags |= ImGuiWindowFlags_NoMove;

        ImVec2 internalPos;
        ImVec2 internalPivot;

        const float scaledDistance = ImGuiWrapper::S(distance);
        float posXLeft = anchorPos.x + scaledDistance;
        float posXRight = anchorPos.x + anchorSize.x - scaledDistance;
        float posYTop = anchorPos.y + scaledDistance;
        float posYDown = anchorPos.y + anchorSize.y - scaledDistance;
        switch (corner)
        {
            case HEngine::WindowCorner::LeftTop:
            {
                internalPos = { posXLeft, posYTop };
                internalPivot = { 0.0f, 0.0f };
                break;
            }
            case HEngine::WindowCorner::RightTop:
            {
                internalPos = { posXRight, posYTop };
                internalPivot = { 1.0f, 0.0f };
                break;
            }
            case HEngine::WindowCorner::LeftDown:
            {
                internalPos = { posXLeft, posYDown };
                internalPivot = { 0.0f, 1.0f };
                break;
            }
            case HEngine::WindowCorner::RightDown:
            {
                internalPos = { posXRight, posYDown };
                internalPivot = { 1.0f, 1.0f };
                break;
            }
            default:
            {
                internalPos = { posXLeft, posYTop };
                internalPivot = { 0.0f, 0.0f };
                break;
            }
        }

        ImGui::SetNextWindowPos(internalPos, ImGuiCond_Always, internalPivot);
        if (viewport)
            ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::SetNextWindowBgAlpha(0.0f); // Transparent background

        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f); // 无边框

        if (ImGui::Begin(name, p_open, windowFlags))
        {
            uiFunc();
        }
        ImGui::End();

        ImGui::PopStyleVar();
    }
}
