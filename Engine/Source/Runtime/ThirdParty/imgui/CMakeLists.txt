#设置最小版本
cmake_minimum_required(VERSION 3.5)
project(ImGui)

file(GLOB ImGuiSrc
"imconfig.h"
"imgui.h"
"imgui.cpp"
"imgui_draw.cpp"
"imgui_internal.h"
"imgui_tables.cpp"
"imgui_widgets.cpp"
"imstb_rectpack.h"
"imstb_textedit.h"
"imstb_truetype.h"
"imgui_demo.cpp"
)

add_library(ImGui STATIC ${ImGuiSrc})


