set(ImGuizmoSourceDir ${ThirdPartyDir}/ImGuizmo)

set(ImGuizmoSrc 
    "${ImGuizmoSourceDir}/ImGuizmo.h"
    "${ImGuizmoSourceDir}/ImGuizmo.cpp"
)

add_library(ImGuizmo STATIC ${ImGuizmoSrc})
target_include_directories(ImGuizmo PUBLIC "${ImGuizmoSourceDir}")
target_compile_definitions(ImGuizmo PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(ImGuizmo PUBLIC imgui)
