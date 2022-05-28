macro(RUNTIME_ADD_SOURCE)
	set(SOURCE_FILES "")
    foreach(SOURCE_FILE ${ARGN})
        if(SOURCE_FILE MATCHES "^/.*")
            list(APPEND SOURCE_FILES ${SOURCE_FILE})
        else()
            list(APPEND SOURCE_FILES
                 "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}")
        endif()
    endforeach()
    set(RuntimeSourceFiles ${RuntimeSourceFiles} ${SOURCE_FILES} PARENT_SCOPE)
endmacro(RUNTIME_ADD_SOURCE)

set(RuntimeSourceFiles "${CMAKE_CURRENT_SOURCE_DIR}/HEngine.h" "${CMAKE_CURRENT_SOURCE_DIR}/hepch.h" "${CMAKE_CURRENT_SOURCE_DIR}/hepch.cpp")

add_subdirectory(Animation)
add_subdirectory(Camera)
add_subdirectory(Core)
add_subdirectory(Debug)
add_subdirectory(EcsFramework)
add_subdirectory(Events)
add_subdirectory(ImGui)
add_subdirectory(Input)
add_subdirectory(Launch)
add_subdirectory(Library)
add_subdirectory(Mesh)
add_subdirectory(Platform)
add_subdirectory(Renderer)
add_subdirectory(Resource)
add_subdirectory(Utils)

source_group(TREE "${EngineRuntimeDir}" FILES ${RuntimeSourceFiles})

add_library(HEngineRuntime STATIC ${RuntimeSourceFiles})

target_include_directories(HEngineRuntime 
    PRIVATE "${EngineSourceDir}"
    PRIVATE "${EngineRuntimeDir}" # make sure we can directly include "hepch.h"
    PUBLIC ${vulkan_include}
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
    PUBLIC glm
    PUBLIC imgui
    PUBLIC ${OPENGL_LIBRARIES}
    PUBLIC yaml-cpp
    PUBLIC spdlog
    PUBLIC box2d
    PUBLIC stb
    PUBLIC ImGuizmo
    PUBLIC assimp
    PUBLIC BulletSoftBody
    PUBLIC Bullet3Dynamics 
    PUBLIC BulletDynamics 
    PUBLIC Bullet3Collision 
    PUBLIC BulletCollision 
    PUBLIC Bullet3Common 
    PUBLIC LinearMath 
    PUBLIC shaderc
    PUBLIC spirv-cross-core
    PUBLIC spirv-cross-glsl
    PUBLIC headers
    PUBLIC ${vulkan_lib}
    PUBLIC pybind11::embed pybind11::module pybind11::pybind11
    PUBLIC magic_enum
    PUBLIC FMOD::Core FMOD::Fsbank FMOD::Studio
)

if (WIN32)
    target_link_libraries(HEngineRuntime PUBLIC d3d11.lib dxgi.lib dxguid.lib D3DCompiler.lib d2d1.lib dwrite.lib winmm.lib)
endif()

# sometime will have bugs, why?
# if (DirectX11_FOUND)
#     target_link_libraries(HEngineRuntime PUBLIC ${DirectX11_LIBRARIES})
#     target_include_directories(HEngineRuntime PUBLIC ${DirectX11_INCLUDE_DIRS})
# endif()

