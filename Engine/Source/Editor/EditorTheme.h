#pragma once

#include <imgui.h>

namespace HEngine::EditorTheme
{
    void DrawPill(const char* text,
        const ImVec4& backgroundColor,
        const ImVec4& textColor = ImVec4(0.94f, 0.96f, 0.98f, 1.0f));

    void DrawSectionHeader(const char* title,
        const char* subtitle = nullptr,
        const ImVec4& accentColor = ImVec4(0.30f, 0.55f, 0.85f, 1.0f));
}
