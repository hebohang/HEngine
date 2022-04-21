#pragma once

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

namespace HEngine::ImGuiWrapper
{
    bool InputTextLeft(const char* label, char* buf, size_t buf_size, ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = NULL, void* user_data = NULL);
}