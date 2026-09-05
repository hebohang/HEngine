set(ThirdPartyDir "${EngineSourceDir}/ThirdParty")
set(EngineRuntimeDir "${EngineSourceDir}/Runtime")
set(EngineEditorDir "${EngineSourceDir}/Editor")
set(EnginePluginsDir "${EngineSourceDir}/Plugins")

option(HE_RENDER_DX12 "Compile the DirectX 12 render backend." ON)
option(HE_RENDER_VULKAN "Compile the Vulkan render backend." ON)
if(NOT HE_RENDER_DX12 AND NOT HE_RENDER_VULKAN)
    message(FATAL_ERROR "At least one HEngine render backend must be enabled.")
endif()
if(NOT WIN32 AND HE_RENDER_DX12)
    message(FATAL_ERROR "HE_RENDER_DX12 is only available on Windows.")
endif()

# NVRHI currently requires Vulkan-Headers >= 1.4.318. HEngine vendors the
# exact public headers for v1.4.352 instead of reading an arbitrary developer
# VULKAN_SDK or downloading during configure. An audited checkout override is
# available for dependency-update validation.
set(HE_VULKAN_HEADERS_VERSION "v1.4.352" CACHE STRING
    "Pinned Vulkan-Headers tag used by the HEngine Vulkan backend.")
set(HE_VULKAN_HEADERS_SOURCE_DIR "" CACHE PATH
    "Optional audited Vulkan-Headers source checkout matching HE_VULKAN_HEADERS_VERSION.")
if(HE_RENDER_VULKAN AND HE_VULKAN_HEADERS_SOURCE_DIR)
    get_filename_component(HE_VULKAN_HEADERS_SOURCE_DIR
        "${HE_VULKAN_HEADERS_SOURCE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(NOT EXISTS "${HE_VULKAN_HEADERS_SOURCE_DIR}/CMakeLists.txt" OR
       NOT EXISTS "${HE_VULKAN_HEADERS_SOURCE_DIR}/include/vulkan/vulkan_core.h")
        message(FATAL_ERROR
            "HE_VULKAN_HEADERS_SOURCE_DIR does not contain a Vulkan-Headers checkout: "
            "${HE_VULKAN_HEADERS_SOURCE_DIR}")
    endif()
endif()

option(HE_ENABLE_TRACY "Enable Tracy profiler integration." OFF)
option(HE_TRACY_ON_DEMAND "Collect Tracy data only while the profiler is connected." ON)
option(HE_TRACY_MEMORY_PROFILING "Collect allocator events in Tracy profiles." OFF)
set(HE_TRACY_CALLSTACK_DEPTH "0" CACHE STRING "Tracy callstack depth. Set to 0 to disable callstack capture.")

if(PACK_GAME)
    set(HE_ENABLE_TRACY OFF CACHE BOOL "Disable profiling in packaged games." FORCE)
    set(HE_TRACY_MEMORY_PROFILING OFF CACHE BOOL "Disable memory profiling in packaged games." FORCE)
    set(HE_TRACY_CALLSTACK_DEPTH "0" CACHE STRING "Disable shipping callstack capture." FORCE)
endif()

if(WIN32)
    include(${CMAKE_SOURCE_DIR}/CMake/DirectXAgilitySDK.cmake)
endif()

add_subdirectory(ThirdParty)

# LLGI enables /WX itself. New Clang releases add diagnostics for this vendored
# snapshot; third-party warnings must not become engine build failures. Keep
# the exception local instead of weakening warnings for engine/project code.
if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang" AND TARGET LLGI)
    target_compile_options(LLGI PRIVATE
        "-Wno-error"
        "-Wno-inconsistent-missing-override")
endif()

if(PACK_GAME OR HE_BUILD_DEV_GAME)
    # Game UI only uses RmlUi Core. The convenience interface also links the
    # interactive debugger, which is editor-only overhead.
    if(TARGET rmlui)
        set_property(TARGET rmlui PROPERTY INTERFACE_LINK_LIBRARIES rmlui_core)
    endif()
    if(TARGET rmlui_debugger)
        set_target_properties(rmlui_debugger PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endif()

if(PACK_GAME AND TARGET Jolt)
    foreach(definitionProperty IN ITEMS COMPILE_DEFINITIONS INTERFACE_COMPILE_DEFINITIONS)
        get_target_property(joltDefinitions Jolt ${definitionProperty})
        if(joltDefinitions)
            list(FILTER joltDefinitions EXCLUDE REGEX
                "JPH_(ENABLE_ASSERTS|DEBUG_RENDERER|PROFILE_ENABLED|EXTERNAL_PROFILE)")
            set_property(TARGET Jolt PROPERTY ${definitionProperty} "${joltDefinitions}")
        endif()
    endforeach()
endif()

if((PACK_GAME OR HE_BUILD_DEV_GAME) AND TARGET imgui)
    get_target_property(imguiSources imgui SOURCES)
    list(FILTER imguiSources EXCLUDE REGEX
        "imgui_demo")
    set_property(TARGET imgui PROPERTY SOURCES "${imguiSources}")
endif()

include(${CMAKE_SOURCE_DIR}/CMake/HEnginePlugins.cmake)
if(PACK_GAME)
    # Shipping is deliberately isolated from developer tools and smoke hosts.
    set(HE_ACTIVE_PLUGIN_HOSTS Game)
    set(HE_PLUGIN_BUILD_DISTRIBUTION Shipping)
elseif(HE_BUILD_DEV_GAME)
    set(HE_PLUGIN_BUILD_DISTRIBUTION Development)
    if(HE_BUILD_TESTS)
        set(HE_ACTIVE_PLUGIN_HOSTS Tests Tools RenderSmoke)
    else()
        set(HE_ACTIVE_PLUGIN_HOSTS DevGame Tools RenderSmoke)
    endif()
else()
    set(HE_ACTIVE_PLUGIN_HOSTS Editor Tools RenderSmoke)
    set(HE_PLUGIN_BUILD_DISTRIBUTION Development)
endif()

set(HE_PLUGIN_DISCOVERY_ARGUMENTS
    ROOT "${EnginePluginsDir}"
    CONFIG "${ProjectConfigDir}/Plugins.json"
    GENERATED_DIR "${CMAKE_BINARY_DIR}/generated/plugins"
    ACTIVE_HOSTS ${HE_ACTIVE_PLUGIN_HOSTS}
    PLATFORM Windows
    BUILD_DISTRIBUTION ${HE_PLUGIN_BUILD_DISTRIBUTION})
he_discover_static_plugins(${HE_PLUGIN_DISCOVERY_ARGUMENTS})

add_subdirectory(Plugins)
add_subdirectory(Programs)
add_subdirectory(Runtime)

if (PACK_GAME OR (HE_BUILD_DEV_GAME AND NOT HE_BUILD_TESTS))
    add_subdirectory(Game)
elseif(NOT HE_BUILD_DEV_GAME)
    add_subdirectory(Editor)
endif()

set(CODEGEN_TARGET "HEnginePreCompile")
include(${CMAKE_SOURCE_DIR}/CMake/HEngineParser.cmake)
# if (DEFINED CPP_CODE_GEN)
#     set_target_properties("${CODEGEN_TARGET}" PROPERTIES FOLDER "Engine" )
# endif()

target_include_directories(HEngineRuntime BEFORE PRIVATE
    "${HE_GENERATED_SOURCE_DIR}"
    "${GeneratedSeriFileDir}")

if (TARGET HEngineEditor)
    target_include_directories(HEngineEditor BEFORE PRIVATE
        "${HE_GENERATED_SOURCE_DIR}"
        "${GeneratedSeriFileDir}")
endif()

if (TARGET HGame)
    target_include_directories(HGame BEFORE PRIVATE
        "${HE_GENERATED_SOURCE_DIR}"
        "${GeneratedSeriFileDir}")
endif()

foreach(generatedConsumer IN ITEMS HEngineDevAgentRuntime HEngineTests)
    if(TARGET ${generatedConsumer})
        target_include_directories(${generatedConsumer} BEFORE PRIVATE
            "${HE_GENERATED_SOURCE_DIR}"
            "${GeneratedSeriFileDir}")
    endif()
endforeach()

foreach(PluginName ${PluginNames})
    target_include_directories(${PluginName} BEFORE PRIVATE
        "${HE_GENERATED_SOURCE_DIR}"
        "${GeneratedSeriFileDir}")
endforeach()

# if (DEFINED CPP_CODE_GEN)
#     add_dependencies(HEngineRuntime "${CODEGEN_TARGET}")
#     add_dependencies("${CODEGEN_TARGET}" "HEngineParser")
# endif()

if(MSVC)
    if(TARGET HGame)
        set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT HGame)
    else()
        set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT HEngineEditor)
    endif()
endif()
