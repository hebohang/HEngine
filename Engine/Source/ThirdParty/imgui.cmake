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
"${ImguiSourceDir}/backends/imgui_impl_glfw.cpp" 
"${ImguiSourceDir}/backends/imgui_impl_glfw.h"
"${ImguiSourceDir}/backends/imgui_impl_opengl3.cpp" 
"${ImguiSourceDir}/backends/imgui_impl_opengl3.h"
"${ImguiSourceDir}/backends/imgui_impl_vulkan.cpp" 
"${ImguiSourceDir}/backends/imgui_impl_vulkan.h"
)

add_library(imgui STATIC ${ImGuiSrc} ${ImGuiImpl})
target_include_directories(imgui 
    PUBLIC "${ThirdPartyDir}"
    PUBLIC "${ImguiSourceDir}"
    PUBLIC "${vulkan_include}"
)
target_link_libraries(imgui PUBLIC glfw Glad ${OPENGL_LIBRARIES})
