file(GLOB_RECURSE RuntimeSrc
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
)

add_library(HEngineRuntime STATIC ${RuntimeSrc})

target_include_directories(HEngineRuntime PRIVATE
    ""
    "HEngine"
)

target_compile_definitions(HEngineRuntime
    PRIVATE HE_PLATFORM_WINDOWS
    PRIVATE HE_BUILD_DLL
    PRIVATE GLFW_INCLUDE_NONE
    PRIVATE IMGUI_IMPL_OPENGL_LOADER_CUSTOM
    PRIVATE HE_DEBUG
)

set_source_files_properties(ThirdParty/imguizmo/imGuizmo.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

#Ô¤±àÒëÍ·ÎÄ¼þ
target_precompile_headers(HEngineRuntime
PUBLIC "hepch.h"
)

target_link_libraries(HEngineRuntime PUBLIC glfw
    PUBLIC Glad
    PUBLIC ${OPENGL_LIBRARIES}
    PUBLIC yaml-cpp
    PUBLIC spdlog
    PUBLIC glm
    PUBLIC entt
    PUBLIC Box2D
    PUBLIC stb
    PUBLIC ImGuizmo
)



