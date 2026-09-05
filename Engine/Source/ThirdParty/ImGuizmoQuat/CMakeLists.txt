set(ImGuizmoQuat_DIR ${ThirdPartyDir}/ImGuizmoQuat)

add_library(ImGuizmoQuat STATIC 
    ${ImGuizmoQuat_DIR}/imGuIZMOquat.cpp
    ${ImGuizmoQuat_DIR}/imGuIZMOquat.h
    ${ImGuizmoQuat_DIR}/vgConfig.h
    ${ImGuizmoQuat_DIR}/vGizmo.h
    ${ImGuizmoQuat_DIR}/vGizmoMath.h
    ${ImGuizmoQuat_DIR}/vgMath.h
)
target_link_libraries(ImGuizmoQuat PUBLIC imgui)
