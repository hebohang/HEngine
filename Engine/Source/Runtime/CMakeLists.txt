macro(RUNTIME_ADD_SOURCE)
	set(HEADER_FILES "")
	set(SOURCE_FILES "")
    foreach(SOURCE_FILE ${ARGN})
        if(SOURCE_FILE MATCHES "(\.h$)|(\.hpp$)")
            list(APPEND HEADER_FILES
                 "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}")
        elseif(SOURCE_FILE MATCHES "(\.cpp$)|(\.cc$)")
            list(APPEND SOURCE_FILES
                 "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}")
        endif()
    endforeach()
    set(RuntimeHeaderFiles ${RuntimeHeaderFiles} ${HEADER_FILES} PARENT_SCOPE)
    set(RuntimeSourceFiles ${RuntimeSourceFiles} ${SOURCE_FILES} PARENT_SCOPE)
endmacro(RUNTIME_ADD_SOURCE)

set(RuntimeHeaderFiles "${CMAKE_CURRENT_SOURCE_DIR}/HEngine.h" "${CMAKE_CURRENT_SOURCE_DIR}/Hepch.h")
set(RuntimeSourceFiles "${CMAKE_CURRENT_SOURCE_DIR}/Hepch.cpp")

add_subdirectory(Animation)
add_subdirectory(AI)
add_subdirectory(Audio)
add_subdirectory(Camera)
add_subdirectory(Core)
if(HE_ENABLE_GM)
    add_subdirectory(DevTools)
endif()
add_subdirectory(Profile)
add_subdirectory(EcsFramework)
add_subdirectory(Events)
add_subdirectory(ImGui)
add_subdirectory(Launch)
add_subdirectory(Library)
add_subdirectory(Platform)
add_subdirectory(Graphics)
add_subdirectory(Resource)
add_subdirectory(Scripting)
add_subdirectory(UI)
add_subdirectory(Utils)

source_group(TREE "${EngineRuntimeDir}" FILES ${RuntimeHeaderFiles} ${RuntimeSourceFiles})

add_library(HEngineRuntime STATIC ${RuntimeHeaderFiles} ${RuntimeSourceFiles})

target_include_directories(HEngineRuntime 
    PRIVATE "${EngineSourceDir}"
    PRIVATE "${EngineRuntimeDir}" # make sure we can directly include "Hepch.h"
    PRIVATE "${ThirdPartyDir}/Effekseer/src/Effekseer"
    PRIVATE "${ThirdPartyDir}/Effekseer/src/EffekseerRendererDX12"
    PRIVATE "${ThirdPartyDir}/Effekseer/src/EffekseerRendererVulkan"
    PUBLIC "${HE_ENGINE_GENERATED_INCLUDE_DIR}"
)

target_compile_definitions(HEngineRuntime
    PRIVATE HE_BUILD_DLL
    PRIVATE GLFW_INCLUDE_NONE
    PUBLIC HE_ENABLE_GM=$<BOOL:${HE_ENABLE_GM}>
    PRIVATE HE_NVRHI_NVAPI_ENABLED=$<BOOL:${NVRHI_WITH_NVAPI}>
    PRIVATE HE_NVRHI_NATIVE_OMM_ENABLED=$<BOOL:${NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP}>
    PUBLIC HE_RENDER_DX12=$<BOOL:${HE_RENDER_DX12}>
    PUBLIC HE_RENDER_VULKAN=$<BOOL:${HE_RENDER_VULKAN}>
)
if(HE_BUILD_TESTS)
    # Tests do not create HGame/AgentRuntime, but they compile and exercise the
    # same synthetic input implementation used by the Dev Agent Runtime.
    target_compile_definitions(HEngineRuntime PUBLIC HE_DEV_AGENT_RUNTIME=1)
endif()

if(PACK_GAME)
    set(HE_PACKAGE_ZIP_SOURCES
        "${ThirdPartyDir}/assimp/contrib/unzip/ioapi.c"
        "${ThirdPartyDir}/assimp/contrib/unzip/unzip.c"
        "${ThirdPartyDir}/assimp/contrib/zip/src/zip.c"
    )
    target_sources(HEngineRuntime PRIVATE ${HE_PACKAGE_ZIP_SOURCES})
    set_source_files_properties(${HE_PACKAGE_ZIP_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    set_source_files_properties(
        "${ThirdPartyDir}/assimp/contrib/zip/src/zip.c"
        PROPERTIES COMPILE_DEFINITIONS MINIZ_NO_TIME)
    target_compile_definitions(HEngineRuntime PRIVATE HE_PACKAGED_GAME=1)
    target_include_directories(HEngineRuntime PRIVATE
        "${ThirdPartyDir}/assimp/contrib/unzip"
        "${ThirdPartyDir}/assimp/contrib/zip/src"
        "${ThirdPartyDir}/assimp/contrib/zlib"
        "${CMAKE_CURRENT_BINARY_DIR}/../ThirdParty/assimp/contrib/zlib"
    )
    target_link_libraries(HEngineRuntime PRIVATE zlibstatic)
    if(WIN32)
        target_link_libraries(HEngineRuntime PRIVATE Shell32 Ole32)
    endif()
endif()

target_compile_definitions(HEngineRuntime PUBLIC $<$<CONFIG:Debug>:HE_DEBUG=1>)
if(PACK_GAME)
    target_compile_definitions(HEngineRuntime PRIVATE HE_VFX_DETAILED_STATS=0)
else()
    target_compile_definitions(HEngineRuntime PRIVATE HE_VFX_DETAILED_STATS=1)
endif()

if(HE_BUILD_DEV_GAME AND NOT PACK_GAME)
    target_compile_definitions(HEngineRuntime PRIVATE HE_VFX_GPU_STRESS_ALLOWED=1)
else()
    target_compile_definitions(HEngineRuntime PRIVATE HE_VFX_GPU_STRESS_ALLOWED=0)
endif()

if (MSVC)
    target_compile_options(HEngineRuntime PRIVATE /bigobj)
endif()

target_precompile_headers(HEngineRuntime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Hepch.h")

target_link_libraries(HEngineRuntime PUBLIC glfw
    PUBLIC glm
    PUBLIC imgui
    PUBLIC spdlog
    PUBLIC box2d
    PUBLIC stb
    PUBLIC Jolt
    PUBLIC pybind11::embed pybind11::module pybind11::pybind11
    PUBLIC magic_enum
    PUBLIC nlohmann_json::nlohmann_json
    PUBLIC xg
    PUBLIC simpleIni
    PUBLIC headers
    PUBLIC libs
    PUBLIC Tracy::TracyClient
    PRIVATE miniaudio
    PUBLIC Effekseer
    PRIVATE EffekseerRendererDX12
    PRIVATE EffekseerRendererVulkan
    PUBLIC Recast
    PUBLIC Detour
    PUBLIC DetourCrowd
    PUBLIC DetourTileCache
    PUBLIC rmlui
    PUBLIC nvrhi
)

if(HE_RENDER_DX12)
    target_link_libraries(HEngineRuntime PRIVATE nvrhi_d3d12)
endif()
if(HE_RENDER_VULKAN)
    target_link_libraries(HEngineRuntime PRIVATE nvrhi_vk)
    if(TARGET Vulkan::Headers)
        target_link_libraries(HEngineRuntime PRIVATE Vulkan::Headers)
    elseif(TARGET Vulkan-Headers)
        target_link_libraries(HEngineRuntime PRIVATE Vulkan-Headers)
    else()
        message(FATAL_ERROR
            "HE_RENDER_VULKAN requires the pinned Vulkan-Headers target produced by NVRHI.")
    endif()
endif()

if(NOT PACK_GAME)
    target_link_libraries(HEngineRuntime PUBLIC
        ImGuizmo
        embree
        assimp
    )

    if(NOT HE_BUILD_DEV_GAME)
        target_link_libraries(HEngineRuntime PUBLIC
            imgui_node_editor
            ImGuizmoQuat
            imgui_notify
        )
    endif()
endif()

set_target_properties(HEngineRuntime PROPERTIES FOLDER "Engine")

if (WIN32)
    # Native DX12 libraries are an implementation detail of Runtime/Platform.
    target_link_libraries(HEngineRuntime PRIVATE DirectX)
    target_link_libraries(HEngineRuntime PRIVATE windowscodecs ole32)
    if(MSVC AND HE_EFFEKSEER_BUILD_VULKAN)
        # Vendored Effekseer/LLGI still calls Vulkan's C entry points. Keep
        # those imports delayed so the default DX12 path starts on machines
        # without a Vulkan loader; the Vulkan backend preloads the System32
        # loader before any of these functions are used.
        target_link_options(HEngineRuntime INTERFACE "/DELAYLOAD:vulkan-1.dll")
        target_link_libraries(HEngineRuntime INTERFACE delayimp)
    endif()
endif()

if(HE_BUILD_TESTS)
    add_subdirectory(UnitTest)
endif()


set(HEngineRuntimeHeaders "${RuntimeHeaderFiles}" PARENT_SCOPE)
