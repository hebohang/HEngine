file(GLOB_RECURSE RuntimeHeaderFiles CONFIGUE_DEPENDS "*.h")
file(GLOB_RECURSE RuntimeSourceFiles CONFIGUE_DEPENDS "*.cpp")

source_group(TREE "${EngineRuntimeDir}" FILES ${RuntimeHeaderFiles} ${RuntimeSourceFiles})

add_library(HEngineRuntime STATIC ${RuntimeHeaderFiles} ${RuntimeSourceFiles})

target_include_directories(HEngineRuntime 
    PRIVATE "${EngineSourceDir}"
    PRIVATE "${EngineRuntimeDir}" # make sure we can directly include "hepch.h"
)

target_compile_definitions(HEngineRuntime
    PRIVATE HE_BUILD_DLL
    PRIVATE GLFW_INCLUDE_NONE
    PRIVATE IMGUI_IMPL_OPENGL_LOADER_CUSTOM
    PRIVATE HE_DEBUG
    "ENGINE_ROOT_DIR=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)

target_precompile_headers(HEngineRuntime PUBLIC "hepch.h")

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



