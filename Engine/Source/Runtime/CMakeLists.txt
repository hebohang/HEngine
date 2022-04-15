add_subdirectory(ThirdParty)

file(GLOB_RECURSE HEngineSrc
"HEngine/*.h"
"HEngine/*.cpp"
"HEngine/Core/*.h"
"HEngine/Events/*.h"
"HEngine/ImGui/*.h"
"HEngine/Renderer/*.h"
"HEngine/Debug/*.h"
"HEngine/Scene/*.h"
"HEngine/Utils/*.h"
"HEngine/Math/*.h"
"HEngine/Core/*.cpp"
"HEngine/Events/*.cpp"
"HEngine/ImGui/*.cpp"
"HEngine/Renderer/*.cpp"
"HEngine/Debug/*.cpp"
"HEngine/Scene/*.cpp"
"HEngine/Utils/*.cpp"
"HEngine/Math/*.cpp"
"Platform/OpenGL/*.h"
"Platform/OpenGL/*.cpp"
"Platform/Windows/*.h"
"Platform/Windows/*.cpp"
"ThirdParty/stb_image/*.h"
"ThirdParty/stb_image/*.cpp"
"ThirdParty/glm/glm/*.hpp"
"ThirdParty/glm/glm/*.inl"
"ThirdParty/entt/include/*.hpp"
"ThirdParty/imguizmo/ImGuizmo.h"
"ThirdParty/imguizmo/ImGuizmo.cpp"
"ThirdParty/Box2D/include/*.h"
"ThirdParty/Box2D/src/*.cpp"
)

source_group(TREE ${CMAKE_SOURCE_DIR}/Engine/Source/Runtime FILES ${HEngineSrc})

add_library(HEngineRuntime STATIC ${HEngineSrc})

#链接了HEngine的模块，不会包含这些文件
target_include_directories(HEngineRuntime PRIVATE
""
"HEngine"
"ThirdParty/spdlog/include"
"ThirdParty/GLFW/include"
"ThirdParty/Glad/include"
"ThirdParty/imgui"
"ThirdParty/glm"
"ThirdParty/stb_image"
"ThirdParty/entt/include"
"ThirdParty/imguizmo"
"ThirdParty/Box2D/include"
)

target_compile_definitions(HEngineRuntime
    PRIVATE HE_PLATFORM_WINDOWS
    PRIVATE HE_BUILD_DLL
    PRIVATE GLFW_INCLUDE_NONE
    PRIVATE IMGUI_IMPL_OPENGL_LOADER_CUSTOM
    PRIVATE HE_DEBUG
)

set_source_files_properties(ThirdParty/imguizmo/imGuizmo.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

#预编译头文件
target_precompile_headers(HEngineRuntime
PUBLIC "hepch.h"
)

find_package(OpenGL 4.5 REQUIRED)

if(OPENGL_FOUND)
    message("Found OpenGL")
else()
    message("Not Found OpenGL")
endif()

target_link_libraries(HEngineRuntime PUBLIC glfw
    PUBLIC GLAD
    PUBLIC ImGui
    PUBLIC ${OPENGL_LIBRARIES}
    PUBLIC yaml-cpp
)



