set(ImguiSourceDir ${ThirdPartyDir}/imgui)

set(ImGuiSrc
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
set(ImGuiImpl  
"${ImguiSourceDir}/backends/imgui_impl_glfw.cpp" 
"${ImguiSourceDir}/backends/imgui_impl_glfw.h"
)

add_library(imgui STATIC ${ImGuiSrc} ${ImGuiImpl})
target_include_directories(imgui 
    PUBLIC "${ThirdPartyDir}"
    PUBLIC "${ImguiSourceDir}"
)
target_link_libraries(imgui PUBLIC glfw)
