set(ImGuiNodeEditorSourceDir ${ThirdPartyDir}/imgui-node-editor)

set(ImGuiNodeEditorSrc
    "${ImGuiNodeEditorSourceDir}/imgui_node_editor.h"
    "${ImGuiNodeEditorSourceDir}/imgui_node_editor_api.cpp"
    "${ImGuiNodeEditorSourceDir}/imgui_node_editor.cpp"
    "${ImGuiNodeEditorSourceDir}/imgui_node_editor_internal.h"
    "${ImGuiNodeEditorSourceDir}/imgui_node_editor_internal.inl"
    "${ImGuiNodeEditorSourceDir}/imgui_canvas.h"
    "${ImGuiNodeEditorSourceDir}/imgui_canvas.cpp"
    "${ImGuiNodeEditorSourceDir}/imgui_extra_math.h"
    "${ImGuiNodeEditorSourceDir}/imgui_extra_math.inl"
    "${ImGuiNodeEditorSourceDir}/imgui_bezier_math.h"
    "${ImGuiNodeEditorSourceDir}/imgui_bezier_math.inl"
    "${ImGuiNodeEditorSourceDir}/crude_json.h"
    "${ImGuiNodeEditorSourceDir}/crude_json.cpp"
)

add_library(imgui_node_editor STATIC ${ImGuiNodeEditorSrc})
target_include_directories(imgui_node_editor PUBLIC "${ImGuiNodeEditorSourceDir}")
target_link_libraries(imgui_node_editor PUBLIC imgui)
set_target_properties(imgui_node_editor PROPERTIES DEBUG_POSTFIX "d")
