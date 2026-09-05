set(ThirdPartyFolder "ThirdParty")

include(${RootDir}/CMake/Tools.cmake)

option(HE_ENABLE_TRACY "Enable Tracy profiler integration." ON)
option(HE_TRACY_ON_DEMAND "Collect Tracy data only while the profiler is connected." OFF)
option(HE_TRACY_MEMORY_PROFILING "Collect allocator events in Tracy profiles." OFF)
set(HE_TRACY_CALLSTACK_DEPTH "0" CACHE STRING "Tracy callstack depth. Set to 0 to disable callstack capture.")

if(NOT TARGET TracyClient)
    if(HE_ENABLE_TRACY)
        add_library(TracyClient STATIC "${ThirdPartyDir}/Tracy/public/TracyClient.cpp")
        target_include_directories(TracyClient PUBLIC "${ThirdPartyDir}/Tracy/public")
        target_compile_definitions(TracyClient PUBLIC TRACY_ENABLE)

        if(HE_TRACY_ON_DEMAND)
            target_compile_definitions(TracyClient PUBLIC TRACY_ON_DEMAND)
        endif()

        if(HE_TRACY_CALLSTACK_DEPTH)
            target_compile_definitions(TracyClient PUBLIC TRACY_CALLSTACK=${HE_TRACY_CALLSTACK_DEPTH})
        endif()

        if(WIN32)
            target_link_libraries(TracyClient PUBLIC ws2_32 advapi32 user32 dbghelp)
        endif()
    else()
        add_library(TracyClient INTERFACE)
        target_include_directories(TracyClient INTERFACE "${ThirdPartyDir}/Tracy/public")
    endif()

    add_library(Tracy::TracyClient ALIAS TracyClient)
    set_target_properties(TracyClient PROPERTIES FOLDER ${ThirdPartyFolder}/Tracy)
endif()

# GLFW with the same name of project GLFW, so named "glfw"
if(NOT TARGET glfw)
    option(GLFW_BUILD_EXAMPLES "" OFF)
    option(GLFW_BUILD_TESTS "" OFF)
    option(GLFW_BUILD_DOCS "" OFF)
    option(GLFW_INSTALL "" OFF)
    add_subdirectory(glfw)
    set_target_properties(glfw PROPERTIES FOLDER ${ThirdPartyFolder}/GLFW)
    NoWarning(glfw)
endif()

if(NOT TARGET glm)
    option(BUILD_STATIC_LIBS "" ON)
    option(BUILD_TESTING "" OFF)
    option(GLM_TEST_ENABLE "" OFF)
    set(CMAKE_FOLDER ${ThirdPartyFolder}/glm)
    add_subdirectory(glm)
    NoWarningInterface(glm)
    set(GlmHeaderToParse "${ThirdPartyDir}/glm/glm/glm.hpp" PARENT_SCOPE)  # clang parse need the implemented type info
endif()

if(NOT TARGET freetype)
    set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    set(CMAKE_FOLDER ${ThirdPartyFolder}/freetype2)
    add_subdirectory(freetype2)
endif()

if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

if(NOT TARGET imgui)
    include(imgui.cmake)
    set_target_properties(imgui PROPERTIES FOLDER ${ThirdPartyFolder}/imgui)
    NoWarning(imgui)
endif()

if(NOT TARGET spdlog)
    option(SPDLOG_BUILD_EXAMPLE "" OFF)
    option(SPDLOG_INSTALL "" OFF)
    if(WIN32)
        set(SPDLOG_WCHAR_FILENAMES ON CACHE BOOL
            "Use native UTF-16 paths for spdlog file sinks on Windows." FORCE)
    endif()
    add_subdirectory(spdlog)
    set_target_properties(spdlog PROPERTIES FOLDER ${ThirdPartyFolder}/spdlog)
    NoWarning(spdlog)
endif()

if(NOT TARGET box2d)
    option(BOX2D_BUILD_UNIT_TESTS "Build the Box2D unit tests" OFF)
    option(BOX2D_BUILD_TESTBED "Build the Box2D testbed" OFF)
    add_subdirectory(box2d)
    set_target_properties(box2d PROPERTIES FOLDER ${ThirdPartyFolder}/box2d)
    NoWarning(box2d)
endif()

if(NOT TARGET stb)
    include(stb.cmake)
    set_target_properties(stb PROPERTIES FOLDER ${ThirdPartyFolder}/stb)
    NoWarning(stb)
endif()

if(NOT TARGET ImGuizmo)
    include(ImGuizmo.cmake)
    set_target_properties(ImGuizmo PROPERTIES FOLDER ${ThirdPartyFolder}/ImGuizmo)
    NoWarning(ImGuizmo)
endif()

if(NOT TARGET assimp)
    OPTION( BUILD_SHARED_LIBS
      "Build package with shared libraries."
      OFF
    )
    OPTION( ASSIMP_BUILD_FRAMEWORK
      "Build package as Mac OS X Framework bundle."
      OFF
    )
    OPTION( ASSIMP_DOUBLE_PRECISION
      "Set to ON to enable double precision processing"
      OFF
    )
    OPTION( ASSIMP_OPT_BUILD_PACKAGES
      "Set to ON to generate CPack configuration files and packaging targets"
      OFF
    )
    OPTION( ASSIMP_ANDROID_JNIIOSYSTEM
      "Android JNI IOSystem support is active"
      OFF
    )
    OPTION( ASSIMP_NO_EXPORT
      "Disable Assimp's export functionality."
      OFF
    )
    set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "Build your own zlib" FORCE)
    OPTION( ASSIMP_BUILD_ASSIMP_TOOLS
      "If the supplementary tools for Assimp are built in addition to the library."
      OFF
    )
    OPTION ( ASSIMP_BUILD_SAMPLES
      "If the official samples are built as well (needs Glut)."
      OFF
    )
    OPTION ( ASSIMP_BUILD_TESTS
      "If the test suite for Assimp is built in addition to the library."
      OFF
    )
    OPTION ( ASSIMP_COVERALLS
      "Enable this to measure test coverage."
      OFF
    )
    OPTION( ASSIMP_INSTALL
      "Disable this if you want to use assimp as a submodule."
      OFF
    )
    set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "Treat all warnings as errors." FORCE)
    OPTION ( ASSIMP_ASAN
      "Enable AddressSanitizer."
      OFF
    )
    OPTION ( ASSIMP_UBSAN
      "Enable Undefined Behavior sanitizer."
      OFF
    )
    OPTION ( ASSIMP_BUILD_DOCS
      "Build documentation using Doxygen."
      OFF
    )
    OPTION( ASSIMP_INJECT_DEBUG_POSTFIX
      "Inject debug postfix in .a/.so/.dll lib names"
      OFF
    )
    OPTION ( ASSIMP_IGNORE_GIT_HASH
       "Don't call git to get the hash."
       OFF
    )
    set(CMAKE_FOLDER ${ThirdPartyFolder}/assimp)
    add_subdirectory(assimp)
    NoWarning(assimp)
endif()

if(NOT TARGET Jolt)
    set(PHYSICS_REPO_ROOT ${ThirdPartyDir}/JoltPhysics)
    set(USE_ASSERTS ON)
    set(JPH_USE_EXTERNAL_PROFILE ${HE_ENABLE_TRACY})
    set(PROFILER_IN_DEBUG_AND_RELEASE ON)
    set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE ON)
    set(ENABLE_OBJECT_STREAM OFF)
    include(${ThirdPartyDir}/JoltPhysics/Jolt/Jolt.cmake)
    set_target_properties(Jolt PROPERTIES FOLDER ${ThirdPartyFolder}/JoltPhysics)
    NoWarning(Jolt)
endif()

if(NOT TARGET pybind11)
    # add_definitions(-DPYBIND11_FINDPYTHON=OFF)
    # find_package(Python COMPONENTS Interpreter Development)
    set(PYBIND11_FINDPYTHON FALSE)
    set(PYBIND11_NOPYTHON  TRUE)
    set(CMAKE_FOLDER ${ThirdPartyFolder}/pybind11)
    add_subdirectory(pybind11)
endif()

if(NOT TARGET magic_enum)
    set(CMAKE_FOLDER ${ThirdPartyFolder}/magic_enum)
    include(magic_enum.cmake)
endif()

if(NOT TARGET nlohmann_json)
    set(CMAKE_FOLDER ${ThirdPartyFolder}/json)
    add_subdirectory(json)
endif()

if(NOT TARGET DirectX)
    set(CMAKE_FOLDER ${ThirdPartyFolder}/Windows/DirectX)
    add_subdirectory(Windows)
endif()

if(NOT TARGET nvrhi)
    if(HE_RENDER_VULKAN AND NOT TARGET Vulkan::Headers AND NOT TARGET Vulkan-Headers)
        set(HE_VULKAN_HEADERS_DIR "${ThirdPartyDir}/Vulkan-Headers")
        if(HE_VULKAN_HEADERS_SOURCE_DIR)
            set(HE_VULKAN_HEADERS_DIR "${HE_VULKAN_HEADERS_SOURCE_DIR}")
        endif()
        file(STRINGS
            "${HE_VULKAN_HEADERS_DIR}/include/vulkan/vulkan_core.h"
            HE_VULKAN_HEADER_VERSION_LINE
            REGEX "^#define VK_HEADER_VERSION [0-9]+$")
        if(NOT HE_VULKAN_HEADER_VERSION_LINE STREQUAL
                "#define VK_HEADER_VERSION 352")
            message(FATAL_ERROR
                "HEngine requires the pinned Vulkan-Headers v1.4.352; found '${HE_VULKAN_HEADER_VERSION_LINE}' in ${HE_VULKAN_HEADERS_DIR}")
        endif()
        add_subdirectory(
            "${HE_VULKAN_HEADERS_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/Vulkan-Headers"
            EXCLUDE_FROM_ALL)
        message(STATUS
            "HEngine Vulkan-Headers: ${HE_VULKAN_HEADERS_DIR} (pinned v1.4.352)")
    endif()
    set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
    set(NVRHI_INSTALL_EXPORTS OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_VALIDATION ON CACHE BOOL "" FORCE)
    set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_DX12 ${HE_RENDER_DX12} CACHE BOOL "" FORCE)
    set(NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP ON CACHE BOOL "" FORCE)
    set(NVRHI_WITH_VULKAN ${HE_RENDER_VULKAN} CACHE BOOL "" FORCE)
    if(HE_RENDER_VULKAN)
        set(NVRHI_FETCH_VULKAN_HEADERS OFF CACHE BOOL "" FORCE)
        set(NVRHI_VULKAN_HEADERS_GIT_REPOSITORY
            "https://github.com/KhronosGroup/Vulkan-Headers.git"
            CACHE STRING "" FORCE)
        set(NVRHI_VULKAN_HEADERS_GIT_TAG
            "${HE_VULKAN_HEADERS_VERSION}"
            CACHE STRING "" FORCE)
        set(NVRHI_VULKAN_HEADERS_FETCH_DIR "" CACHE STRING "" FORCE)
    endif()
    # The pinned Agility SDK module provides the matching runtime headers and
    # the standard targets expected by NVRHI.
    set(NVRHI_FETCH_DIRECTX_HEADERS OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_NVAPI OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_AFTERMATH OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_RTXMU OFF CACHE BOOL "" FORCE)

    add_subdirectory(NVRHI)
    set(nvrhiTargets nvrhi)
    if(TARGET nvrhi_d3d12)
        list(APPEND nvrhiTargets nvrhi_d3d12)
    endif()
    if(TARGET nvrhi_vk)
        list(APPEND nvrhiTargets nvrhi_vk)
    endif()
    set_target_properties(${nvrhiTargets} PROPERTIES FOLDER ${ThirdPartyFolder}/NVRHI)
    if(TARGET nvrhi_d3d12 AND HE_D3D12_AGILITY_SDK_VERSION LESS 719)
        # This NVRHI snapshot names the preview-only linear-algebra sync bit
        # even when NVRHI_D3D12_WITH_LINALG=0. Stable SDK 619 has no such enum;
        # map it conservatively without editing the vendored source. HEngine
        # does not expose or schedule cooperative-vector/LinAlg work today.
        target_compile_definitions(nvrhi_d3d12 PRIVATE
            D3D12_BARRIER_SYNC_CONVERT_LINEAR_ALGEBRA_MATRIX=D3D12_BARRIER_SYNC_ALL)
    endif()
    NoWarning(nvrhi)
    if(TARGET nvrhi_d3d12)
        NoWarning(nvrhi_d3d12)
    endif()
    if(TARGET nvrhi_vk)
        NoWarning(nvrhi_vk)
    endif()
endif()

if(NOT TARGET xg)
    set(CMAKE_FOLDER ${ThirdPartyFolder}/CrossGuid)
    add_subdirectory(crossguid)
endif()

if(NOT TARGET simpleIni)
    set(CMAKE_FOLDER ${ThirdPartyFolder}/simpleIni)
    add_subdirectory(simpleIni)
endif()

if(NOT TARGET imgui_notify)
    include(imgui_notify.cmake)
    set_target_properties(imgui_notify PROPERTIES FOLDER ${ThirdPartyFolder}/imgui-notify-v2)
endif()

if(NOT TARGET imgui_node_editor)
    include(imgui_node_editor.cmake)
    set_target_properties(imgui_node_editor PROPERTIES FOLDER ${ThirdPartyFolder}/imgui-node-editor)
    NoWarning(imgui_node_editor)
endif()

if(NOT TARGET ImGuizmoQuat)
    add_subdirectory(ImGuizmoQuat)
    set_target_properties(ImGuizmoQuat PROPERTIES FOLDER ${ThirdPartyFolder}/ImGuizmoQuat)
endif()

if(NOT TARGET miniaudio)
    add_subdirectory(miniaudio)
    set_target_properties(miniaudio PROPERTIES FOLDER ${ThirdPartyFolder}/miniaudio)
endif()

if(NOT TARGET embree)
    add_subdirectory(embree)
    set_target_properties(embree PROPERTIES FOLDER ${ThirdPartyFolder}/embree)
endif()

if(NOT TARGET Effekseer)
    set(HE_EFFEKSEER_BUILD_DX12 ${HE_RENDER_DX12} CACHE BOOL
        "Build Effekseer DX12 runtime" FORCE)
    set(HE_EFFEKSEER_BUILD_VULKAN ${HE_RENDER_VULKAN} CACHE BOOL
        "Build Effekseer Vulkan runtime" FORCE)

    if(NOT HE_EFFEKSEER_BUILD_DX12)
        message(FATAL_ERROR
            "HE_EFFEKSEER_BUILD_DX12=OFF is incompatible with the DX12 renderer migration. "
            "Reconfigure this build directory with -DHE_EFFEKSEER_BUILD_DX12=ON.")
    endif()
    set(BUILD_VIEWER OFF CACHE BOOL "" FORCE)
    set(BUILD_EDITOR OFF CACHE BOOL "" FORCE)
    set(BUILD_TEST OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(NETWORK_ENABLED OFF CACHE BOOL "" FORCE)
    set(BUILD_DX9 OFF CACHE BOOL "" FORCE)
    set(BUILD_DX11 OFF CACHE BOOL "" FORCE)
    set(BUILD_GL OFF CACHE BOOL "" FORCE)
    # A normal variable intentionally shadows Effekseer's generic BUILD_DX12
    # cache entry without rewriting user cache state. HE_EFFEKSEER_BUILD_DX12
    # is the engine-owned configuration switch checked above.
    set(BUILD_DX12 ${HE_EFFEKSEER_BUILD_DX12})
    set(BUILD_VULKAN ${HE_EFFEKSEER_BUILD_VULKAN})
    set(BUILD_METAL OFF CACHE BOOL "" FORCE)
    set(USE_DSOUND OFF CACHE BOOL "" FORCE)
    set(USE_OPENAL OFF CACHE BOOL "" FORCE)
    set(USE_OSM OFF CACHE BOOL "" FORCE)
    set(USE_XAUDIO2 OFF CACHE BOOL "" FORCE)
    set(USE_MSVC_RUNTIME_LIBRARY_DLL OFF CACHE BOOL "" FORCE)

    set(CMAKE_FOLDER ${ThirdPartyFolder}/Effekseer)
    add_subdirectory(Effekseer)
endif()

if(NOT TARGET EffekseerRendererDX12)
    message(FATAL_ERROR
        "The DX12 renderer migration requires the EffekseerRendererDX12 CMake target. "
        "Reconfigure with -DHE_EFFEKSEER_BUILD_DX12=ON or provide that target explicitly.")
endif()

if(HE_EFFEKSEER_BUILD_VULKAN AND NOT TARGET EffekseerRendererVulkan)
    message(FATAL_ERROR
        "HE_EFFEKSEER_BUILD_VULKAN=ON requires the EffekseerRendererVulkan CMake target.")
endif()

if(HE_EFFEKSEER_BUILD_VULKAN)
    # Effekseer's Vulkan material compiler is part of the renderer target, but
    # upstream leaves its LLGI compiler disabled unless viewer tooling is built.
    # Enable that already-vendored compiler directly; it uses the bundled
    # glslang targets and does not add SPIRV-Cross to HEngine's shader pipeline.
    target_compile_definitions(LLGI PRIVATE
        ENABLE_VULKAN_COMPILER
        ENABLE_GLSLANG_WITHOUT_INSTALL)
    target_include_directories(LLGI PRIVATE
        "${ThirdPartyDir}/Effekseer/src/3rdParty/glslang"
        "${ThirdPartyDir}/Effekseer/src/3rdParty")
    target_link_libraries(LLGI PRIVATE
        $<BUILD_INTERFACE:Vulkan::Headers>
        $<BUILD_INTERFACE:glslang>
        $<BUILD_INTERFACE:SPIRV>
        $<BUILD_INTERFACE:glslang-default-resource-limits>)
    target_link_libraries(EffekseerRendererVulkan PRIVATE
        $<BUILD_INTERFACE:Vulkan::Headers>)
    NoWarning(EffekseerRendererVulkan)
endif()

# Effekseer's DX12 layers include <d3d12.h> directly. Compile them against the
# same NuGet header set as NVRHI and the engine instead of the inbox SDK copy.
foreach(effekseerDX12Target IN ITEMS LLGI EffekseerRendererLLGI EffekseerRendererDX12)
    if(TARGET ${effekseerDX12Target})
        target_include_directories(${effekseerDX12Target} SYSTEM BEFORE PRIVATE
            "${HE_D3D12_AGILITY_COMPAT_INCLUDE_DIR}"
            "${HE_D3D12_AGILITY_INCLUDE_DIR}")
    endif()
endforeach()

if(NOT TARGET Recast)
    set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
    set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
    set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CMAKE_FOLDER ${ThirdPartyFolder}/recastnavigation)
    add_subdirectory(recastnavigation)
    NoWarning(Recast)
    NoWarning(Detour)
    NoWarning(DetourCrowd)
    NoWarning(DetourTileCache)
    NoWarning(DebugUtils)
endif()

if(NOT TARGET rmlui)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(PLUTOVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LOTTIE_MODULE OFF CACHE BOOL "" FORCE)
    set(LOTTIE_THREAD ON CACHE BOOL "" FORCE)
    set(LOTTIE_CACHE ON CACHE BOOL "" FORCE)
    set(LOTTIE_TEST OFF CACHE BOOL "" FORCE)
    if(NOT TARGET plutovg)
        set(CMAKE_FOLDER ${ThirdPartyFolder}/RmlUi/PlutoVG)
        add_subdirectory(PlutoVG)
    endif()
    if(NOT TARGET lunasvg)
        set(CMAKE_FOLDER ${ThirdPartyFolder}/RmlUi/LunaSVG)
        add_subdirectory(LunaSVG)
    endif()
    if(NOT TARGET rlottie)
        set(CMAKE_FOLDER ${ThirdPartyFolder}/RmlUi/rlottie)
        add_subdirectory(rlottie)
    endif()
    set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
    set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
    set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
    set(RMLUI_LOTTIE_PLUGIN ON CACHE BOOL "" FORCE)
    set(RMLUI_SVG_PLUGIN ON CACHE BOOL "" FORCE)
    set(RMLUI_HARFBUZZ_SAMPLE OFF CACHE BOOL "" FORCE)
    set(RMLUI_TRACY_PROFILING ${HE_ENABLE_TRACY} CACHE BOOL "" FORCE)
    set(RMLUI_TRACY_MEMORY_PROFILING ${HE_TRACY_MEMORY_PROFILING} CACHE BOOL "" FORCE)
    set(RMLUI_TRACY_CONFIGURATION OFF CACHE BOOL "" FORCE)
    set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
    set(RMLUI_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    set(RMLUI_COMPILER_OPTIONS OFF CACHE BOOL "" FORCE)
    set(CMAKE_FOLDER ${ThirdPartyFolder}/RmlUi)
    add_subdirectory(RmlUi)
endif()

if(NOT TARGET headers)
    add_library(headers INTERFACE)
    target_include_directories(headers INTERFACE "${ThirdPartyDir}")
    target_include_directories(headers INTERFACE "${ThirdPartyDir}/entt/include")
    target_include_directories(headers INTERFACE "${ThirdPartyDir}/assimp/include")
    target_include_directories(headers INTERFACE "${ThirdPartyDir}/JoltPhysics")
    target_include_directories(headers INTERFACE "${ThirdPartyDir}/pybind11/include")
    target_include_directories(headers INTERFACE "${ThirdPartyDir}/Windows/D3DX12/Include")
    target_include_directories(headers INTERFACE "${ThirdPartyDir}/Python/include")
    NoWarningInterface(headers)
endif()

if(NOT TARGET libs)
    add_library(libs INTERFACE)
    target_link_libraries(libs INTERFACE "${ThirdPartyDir}/Python/libs/python311.lib")
    target_link_libraries(libs INTERFACE zlibstatic)
endif()

file(COPY "${ThirdPartyDir}/Python/dlls/python311.dll" DESTINATION ${BINARY_ROOT_DIR})
