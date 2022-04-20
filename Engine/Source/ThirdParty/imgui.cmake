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

# Still some bugs
file(GLOB ImGuiImpl CONFIGURE_DEPENDS  
"${imgui_SOURCE_DIR_}/backends/imgui_impl_glfw.cpp" 
"${imgui_SOURCE_DIR_}/backends/imgui_impl_glfw.h"
"${imgui_SOURCE_DIR_}/backends/imgui_impl_opengl3.cpp" 
"${imgui_SOURCE_DIR_}/backends/imgui_impl_opengl3.h")

add_library(imgui STATIC ${ImGuiSrc} ${ImGuiImpl})
target_include_directories(imgui PUBLIC
    "${ThirdPartyDir}"
    "${ImguiSourceDir}"
)
target_link_libraries(imgui PUBLIC glfw Glad ${OPENGL_LIBRARIES})
