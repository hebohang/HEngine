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
target_compile_definitions(HEngineRuntime PRIVATE HE_FSR31=$<BOOL:${HE_FSR31}>)
target_compile_definitions(HEngineRuntime PRIVATE HE_FSR31_DX12=$<BOOL:${HE_FSR31_DX12}> HE_FSR31_VULKAN=$<BOOL:${HE_FSR31_VULKAN}>)
if(HE_FSR31)
    target_include_directories(HEngineRuntime PRIVATE "${HE_FSR31_ROOT}/ffx-api/include")
endif()
target_link_libraries(HEngineRuntime PRIVATE HEngineScriptCore)

target_include_directories(HEngineRuntime 
    PRIVATE "${EngineSourceDir}"
    PRIVATE "${EngineRuntimeDir}" # make sure we can directly include "Hepch.h"
    PRIVATE "${ProjectRootDir}/Source" # project reflection headers in the selected host's generated aggregate
    PRIVATE "${ThirdPartyDir}/Effekseer/src/Effekseer"
    PRIVATE "${ThirdPartyDir}/Effekseer/src/EffekseerRendererDX12"
    PRIVATE "${ThirdPartyDir}/Effekseer/src/EffekseerRendererVulkan"
    PUBLIC "${HE_ENGINE_GENERATED_INCLUDE_DIR}"
    PUBLIC "${CMAKE_BINARY_DIR}/generated/scripting"
)

target_compile_definitions(HEngineRuntime
    PRIVATE HE_BUILD_DLL
    PRIVATE GLFW_INCLUDE_NONE
    PUBLIC HE_RUNTIME_ASSET_IMPORT=$<BOOL:${HE_RUNTIME_ASSET_IMPORT}>
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
    target_compile_definitions(HEngineRuntime PRIVATE HE_PACKAGED_GAME=1)
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

option(HE_EDITOR_VFX_GPU_PROBE "Allow explicit GPU diagnostics in a non-shipping GM Editor" OFF)
if(HE_EDITOR_VFX_GPU_PROBE AND (PACK_GAME OR HE_BUILD_DEV_GAME OR NOT HE_ENABLE_GM))
    message(FATAL_ERROR "HE_EDITOR_VFX_GPU_PROBE requires non-PACK Editor with HE_ENABLE_GM")
endif()
if((HE_BUILD_DEV_GAME OR HE_EDITOR_VFX_GPU_PROBE) AND NOT PACK_GAME)
    target_compile_definitions(HEngineRuntime PRIVATE HE_VFX_GPU_STRESS_ALLOWED=1)
else()
    target_compile_definitions(HEngineRuntime PRIVATE HE_VFX_GPU_STRESS_ALLOWED=0)
endif()

if (MSVC)
    target_compile_options(HEngineRuntime PRIVATE /bigobj)
endif()

target_precompile_headers(HEngineRuntime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Hepch.h")

target_link_libraries(HEngineRuntime PUBLIC glm
    PUBLIC imgui
    PUBLIC spdlog
    PUBLIC stb
    PUBLIC magic_enum
    PUBLIC nlohmann_json::nlohmann_json
    PUBLIC xg
    PUBLIC simpleIni
    PUBLIC headers
    PRIVATE HEArchive
    PUBLIC Tracy::TracyClient
    PRIVATE miniaudio
    PUBLIC Effekseer
    PUBLIC rmlui
    PUBLIC nvrhi
)

if(APPLE)
    target_compile_definitions(HEngineRuntime PRIVATE
        VK_ENABLE_BETA_EXTENSIONS VK_USE_PLATFORM_MACOS_MVK)
    target_link_libraries(HEngineRuntime PRIVATE Vulkan::Vulkan
        "-framework Cocoa" "-framework CoreFoundation" "-framework CoreAudio"
        "-framework AudioToolbox" "-framework AudioUnit")
endif()

if(ANDROID)
    target_link_libraries(HEngineRuntime PRIVATE android log dl)
else()
    target_link_libraries(HEngineRuntime PUBLIC glfw)
endif()

if(HE_RENDER_DX12)
    target_link_libraries(HEngineRuntime PRIVATE nvrhi_d3d12 EffekseerRendererDX12)
endif()
if(HE_RENDER_VULKAN)
    target_link_libraries(HEngineRuntime PRIVATE nvrhi_vk EffekseerRendererVulkan)
    if(TARGET Vulkan::Headers)
        target_link_libraries(HEngineRuntime PRIVATE Vulkan::Headers)
    elseif(TARGET Vulkan-Headers)
        target_link_libraries(HEngineRuntime PRIVATE Vulkan-Headers)
    else()
        message(FATAL_ERROR
            "HE_RENDER_VULKAN requires the pinned Vulkan-Headers target produced by NVRHI.")
    endif()
endif()

foreach(feature IN ITEMS PHYSICS_2D PHYSICS_3D NAVIGATION UI_SVG UI_LOTTIE)
    target_compile_definitions(HEngineRuntime PUBLIC HE_ENABLE_${feature}=$<BOOL:${HE_ENABLE_${feature}}>)
endforeach()
if(HE_ENABLE_PHYSICS_2D)
    target_link_libraries(HEngineRuntime PUBLIC box2d)
endif()
if(HE_ENABLE_PHYSICS_3D)
    target_link_libraries(HEngineRuntime PUBLIC Jolt)
endif()
if(HE_ENABLE_NAVIGATION)
    target_link_libraries(HEngineRuntime PRIVATE Recast Detour DetourCrowd DetourTileCache)
endif()

if(HE_RUNTIME_ASSET_IMPORT)
    target_link_libraries(HEngineRuntime PUBLIC assimp)
endif()
if(HE_WITH_EDITOR)
    target_link_libraries(HEngineRuntime PRIVATE ImGuizmo embree)

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
    if(HE_RENDER_DX12)
        target_link_libraries(HEngineRuntime PRIVATE DirectX)
    endif()
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

if(APPLE AND HE_WITH_EDITOR)
    # Reuse the existing settings suite with the native Editor build graph;
    # no Window or graphics device is created by this isolated runner.
    add_executable(HEMacRenderSettingsTests EXCLUDE_FROM_ALL
        Platform/macOS/Tests/MacRenderSettingsTests.cpp
        UnitTest/RenderSettingsTests.cpp)
    target_include_directories(HEMacRenderSettingsTests PRIVATE
        "${EngineSourceDir}" "${EngineRuntimeDir}" "${HE_GENERATED_SOURCE_DIR}")
    target_link_libraries(HEMacRenderSettingsTests PRIVATE HEngineRuntime)
    set_target_properties(HEMacRenderSettingsTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${BINARY_ROOT_DIR}" FOLDER "Tests")
endif()


set(HEngineRuntimeHeaders "${RuntimeHeaderFiles}" PARENT_SCOPE)
