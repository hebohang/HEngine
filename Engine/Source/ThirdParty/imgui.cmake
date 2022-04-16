set(ImguiSourceDir ${ThirdPartyDir}/imgui)

file(GLOB ImGuiSrc
    "${ImguiSourceDir}/imconfig.h"
    "${ImguiSourceDir}/imgui.h"
    "${ImguiSourceDir}/imgui.cpp"
    "${ImguiSourceDir}/imgui_draw.cpp"
    "${ImguiSourceDir}/imgui_internal.h"
    "${ImguiSourceDir}/imgui_tables.cpp"
    "${ImguiSourceDir}/imgui_widgets.cpp"
    "${ImguiSourceDir}/imstb_rectpack.h"
    "${ImguiSourceDir}/imstb_textedit.h"
    "${ImguiSourceDir}/imstb_truetype.h"
    "${ImguiSourceDir}/imgui_demo.cpp"
)

add_library(imgui STATIC ${ImGuiSrc})
target_include_directories(imgui PUBLIC
    "${ThirdPartyDir}"
    "${ImguiSourceDir}"
)
