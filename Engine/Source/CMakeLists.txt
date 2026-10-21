set(ThirdPartyDir "${EngineSourceDir}/ThirdParty")
set(EngineRuntimeDir "${EngineSourceDir}/Runtime")
set(EngineEditorDir "${EngineSourceDir}/Editor")
set(EnginePluginsDir "${EngineSourceDir}/Plugins")

foreach(feature IN ITEMS PHYSICS_2D PHYSICS_3D NAVIGATION UI_SVG UI_LOTTIE)
    option(HE_ENABLE_${feature} "Enable HEngine ${feature} support." ON)
endforeach()

option(HE_RENDER_DX12 "Compile the DirectX 12 render backend." ${WIN32})
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

if(WIN32 AND HE_RENDER_DX12)
    include(${CMAKE_SOURCE_DIR}/CMake/DirectXAgilitySDK.cmake)
endif()

set(HE_WITH_EDITOR OFF)
if(NOT PACK_GAME AND NOT HE_BUILD_DEV_GAME)
    set(HE_WITH_EDITOR ON)
endif()
option(HE_BUILD_ASSET_TOOLS "Configure asset importer and cooker hosts." ${HE_WITH_EDITOR})
option(HE_BUILD_RENDER_SMOKES "Configure dedicated render smoke hosts." OFF)
include(FidelityFX)

include(${CMAKE_SOURCE_DIR}/CMake/HEnginePlugins.cmake)
if(PACK_GAME)
    # Shipping is deliberately isolated from developer tools and smoke hosts.
    set(HE_ACTIVE_PLUGIN_HOSTS Game)
    set(HE_PLUGIN_BUILD_DISTRIBUTION Shipping)
elseif(HE_BUILD_DEV_GAME)
    set(HE_PLUGIN_BUILD_DISTRIBUTION Development)
    if(HE_BUILD_TESTS)
        set(HE_ACTIVE_PLUGIN_HOSTS Tests)
    else()
        set(HE_ACTIVE_PLUGIN_HOSTS DevGame)
    endif()
else()
    set(HE_ACTIVE_PLUGIN_HOSTS Editor)
    set(HE_PLUGIN_BUILD_DISTRIBUTION Development)
endif()

if(HE_BUILD_ASSET_TOOLS AND NOT PACK_GAME)
    list(APPEND HE_ACTIVE_PLUGIN_HOSTS Tools)
endif()
if(HE_BUILD_RENDER_SMOKES AND NOT PACK_GAME)
    list(APPEND HE_ACTIVE_PLUGIN_HOSTS RenderSmoke)
endif()

set(HE_PLUGIN_CONFIG "" CACHE FILEPATH "Optional explicit project plugin enablement file")
if(HE_PLUGIN_CONFIG)
    get_filename_component(hePluginConfig "${HE_PLUGIN_CONFIG}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
else()
    set(hePluginConfig "${ProjectConfigDir}/Plugins.json")
endif()
set(HE_PLUGIN_TARGET_PLATFORM Windows)
if(ANDROID)
    set(HE_PLUGIN_TARGET_PLATFORM Android)
elseif(APPLE)
    set(HE_PLUGIN_TARGET_PLATFORM macOS)
endif()
set(HE_PLUGIN_DISCOVERY_ARGUMENTS
    ROOT "${EnginePluginsDir}"
    CONFIG "${hePluginConfig}"
    GENERATED_DIR "${CMAKE_BINARY_DIR}/generated/plugins"
    ACTIVE_HOSTS ${HE_ACTIVE_PLUGIN_HOSTS}
    PLATFORM ${HE_PLUGIN_TARGET_PLATFORM}
    BUILD_DISTRIBUTION ${HE_PLUGIN_BUILD_DISTRIBUTION})
he_discover_static_plugins(${HE_PLUGIN_DISCOVERY_ARGUMENTS})

get_property(HE_ENABLED_PLUGIN_IDS GLOBAL PROPERTY HE_STATIC_PLUGIN_ENABLED_IDS)
option(HE_RUNTIME_ASSET_IMPORT "Enable legacy raw mesh/animation loading in Runtime." ${HE_WITH_EDITOR})
set(HE_NEEDS_ASSIMP ${HE_RUNTIME_ASSET_IMPORT})
if("he.importers" IN_LIST HE_ENABLED_PLUGIN_IDS)
    set(HE_NEEDS_ASSIMP ON)
endif()
if(PACK_GAME AND HE_RUNTIME_ASSET_IMPORT)
    message(FATAL_ERROR "Shipping cannot enable legacy Runtime asset import.")
endif()

if(WIN32 AND HE_WINDOWS_ARCH STREQUAL "arm64")
    include(${CMAKE_SOURCE_DIR}/CMake/WindowsArm64Dependencies.cmake)
endif()
if(APPLE)
    set(heMacDeploymentTarget "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    include(${CMAKE_SOURCE_DIR}/CMake/macOSDependencies.cmake)
endif()
if(ANDROID)
    include(${CMAKE_SOURCE_DIR}/CMake/AndroidDependencies.cmake)
else()
    # External projects add an identity directory to the build path. Keep vendor
    # object paths below the Windows compiler limit without modifying vendors.
    if(HE_PORTABLE_EDITOR OR NOT ProjectRootDir STREQUAL "${RootDir}/Project")
        add_subdirectory(ThirdParty "${CMAKE_BINARY_DIR}/deps")
    else()
        add_subdirectory(ThirdParty)
    endif()
endif()

if(APPLE)
    # Effekseer's nested project forces its own minimum into the global cache.
    # Preserve the engine's selected deployment target for subsequent targets.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${heMacDeploymentTarget}" CACHE STRING
        "Minimum macOS deployment target" FORCE)
    foreach(heVulkanTarget IN ITEMS nvrhi_vk LLGI EffekseerRendererVulkan)
        if(TARGET ${heVulkanTarget})
            # LLGI enables these inside its headers. Define them before any
            # Vulkan C header is included so Vulkan-Hpp sees matching types.
            target_compile_definitions(${heVulkanTarget} PRIVATE
                VK_ENABLE_BETA_EXTENSIONS VK_USE_PLATFORM_MACOS_MVK)
        endif()
    endforeach()
    if(TARGET Recast)
        # Runtime's diagnostic rcContext subclass participates in RTTI.
        target_compile_options(Recast PRIVATE -frtti)
    endif()
    if(TARGET rlottie)
        # This snapshot's NEON path calls 32-bit ARM Pixman assembly. Retain
        # its portable scalar renderer on Apple Silicon until arm64 assembly
        # is provided by the dependency.
        target_compile_options(rlottie PRIVATE -U__ARM_NEON__)
    endif()
endif()

include(${RootDir}/CMake/ImGuiNotifyCompat.cmake)
he_enable_imgui_notify_compat()
include(${RootDir}/CMake/ImGuiGlfwCompat.cmake)
he_enable_imgui_glfw_compat()

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


add_subdirectory(Plugins)
if(ANDROID)
    # Shaders are built by the host pipeline and included in Game.pak.
    add_custom_target(HEngineRuntimeShaders)
else()
    add_subdirectory(Programs)
endif()
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
