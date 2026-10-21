# Platform-specific target composition, kept outside the vendored source tree.
# Desktop-only importers, GLFW, DX12 and tools never enter the Android graph.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
function(he_android_dependency name)
    add_subdirectory("${ThirdPartyDir}/${name}" "${CMAKE_BINARY_DIR}/dependencies/${name}" EXCLUDE_FROM_ALL)
endfunction()

add_library(TracyClient INTERFACE)
target_include_directories(TracyClient INTERFACE "${ThirdPartyDir}/Tracy/public")
add_library(Tracy::TracyClient ALIAS TracyClient)
set(GLM_TEST_ENABLE OFF CACHE BOOL "" FORCE)
he_android_dependency(glm)
set(GlmHeaderToParse "${ThirdPartyDir}/glm/glm/glm.hpp")
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
he_android_dependency(spdlog)
set(BOX2D_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_BUILD_TESTBED OFF CACHE BOOL "" FORCE)
he_android_dependency(box2d)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
he_android_dependency(json)
include("${ThirdPartyDir}/stb.cmake")
include("${ThirdPartyDir}/magic_enum.cmake")
he_android_dependency(simpleIni)
he_android_dependency(crossguid)
# CrossGuid's JNI backend caches a thread-local JNIEnv globally. Engine UUIDs
# are also generated on native/managed workers, so use Android's OS entropy.
get_target_property(androidGuidDefinitions xg COMPILE_DEFINITIONS)
list(REMOVE_ITEM androidGuidDefinitions GUID_ANDROID)
set_property(TARGET xg PROPERTY COMPILE_DEFINITIONS "${androidGuidDefinitions}")
target_sources(xg PRIVATE "${EngineRuntimeDir}/Platform/Android/AndroidGuid.cpp")
he_android_dependency(miniaudio)
add_subdirectory("${ThirdPartyDir}/assimp/contrib/zlib"
    "${CMAKE_BINARY_DIR}/dependencies/zlib" EXCLUDE_FROM_ALL)
target_include_directories(zlibstatic PUBLIC "${CMAKE_BINARY_DIR}/dependencies/zlib")
he_android_dependency(Compression)

set(PHYSICS_REPO_ROOT "${ThirdPartyDir}/JoltPhysics")
set(USE_ASSERTS OFF)
set(JPH_USE_EXTERNAL_PROFILE OFF)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF)
set(ENABLE_OBJECT_STREAM OFF)
include("${ThirdPartyDir}/JoltPhysics/Jolt/Jolt.cmake")

add_library(imgui STATIC
    "${ThirdPartyDir}/imgui/imgui.cpp"
    "${ThirdPartyDir}/imgui/imgui_draw.cpp"
    "${ThirdPartyDir}/imgui/imgui_tables.cpp"
    "${ThirdPartyDir}/imgui/imgui_widgets.cpp")
target_include_directories(imgui PUBLIC "${ThirdPartyDir}" "${ThirdPartyDir}/imgui")

set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
set(NVRHI_INSTALL_EXPORTS OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_VALIDATION ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX12 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_VULKAN ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_NVAPI OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_AFTERMATH OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_RTXMU OFF CACHE BOOL "" FORCE)
set(NVRHI_FETCH_VULKAN_HEADERS OFF CACHE BOOL "" FORCE)
he_android_dependency(Vulkan-Headers)
he_android_dependency(NVRHI)

set(HE_EFFEKSEER_BUILD_DX12 OFF)
set(HE_EFFEKSEER_BUILD_VULKAN ON)
foreach(disabled IN ITEMS BUILD_EXAMPLES BUILD_EXAMPLE BUILD_TEST BUILD_GL
    BUILD_DX9 BUILD_DX11 BUILD_DX12 BUILD_METAL BUILD_TOOL BUILD_EDITOR
    USE_DSOUND USE_OPENAL USE_OSM USE_XAUDIO2)
    set(${disabled} OFF CACHE BOOL "" FORCE)
endforeach()
set(BUILD_VULKAN ON CACHE BOOL "" FORCE)
he_android_dependency(Effekseer)
# HEngine provides the device and surface. LLGI's desktop factories and XCB
# window implementation are not part of this embedded renderer integration.
get_target_property(llgiSources LLGI SOURCES)
list(FILTER llgiSources EXCLUDE REGEX "(PC/|Linux/|LLGI[.]PlatformVulkan[.])")
set_property(TARGET LLGI PROPERTY SOURCES "${llgiSources}")
foreach(vulkanConsumer IN ITEMS LLGI EffekseerRendererVulkan EffekseerRendererLLGI)
    target_link_libraries(${vulkanConsumer} PRIVATE $<BUILD_INTERFACE:Vulkan::Headers>)
    target_compile_options(${vulkanConsumer} PRIVATE
        "-include" "${EngineRuntimeDir}/Platform/Android/AndroidVulkanHeaders.h")
endforeach()
target_include_directories(LLGI BEFORE PRIVATE
    "${ThirdPartyDir}/Effekseer/src/3rdParty/glslang"
    "${ThirdPartyDir}/Effekseer/src/3rdParty")
target_link_libraries(LLGI PRIVATE $<BUILD_INTERFACE:Vulkan::Headers>
    $<BUILD_INTERFACE:glslang> $<BUILD_INTERFACE:SPIRV> $<BUILD_INTERFACE:glslang-default-resource-limits>)
target_link_libraries(EffekseerRendererVulkan PRIVATE $<BUILD_INTERFACE:Vulkan::Headers>)

set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
he_android_dependency(recastnavigation)
# Runtime derives its logging context from rcContext and uses RTTI.
target_compile_options(Recast PRIVATE -frtti)
foreach(disabled IN ITEMS FT_DISABLE_ZLIB FT_DISABLE_BZIP2 FT_DISABLE_PNG
    FT_DISABLE_BROTLI FT_DISABLE_HARFBUZZ)
    set(${disabled} ON CACHE BOOL "" FORCE)
endforeach()
he_android_dependency(freetype2)
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()
foreach(disabled IN ITEMS PLUTOVG_BUILD_EXAMPLES LUNASVG_BUILD_EXAMPLES
    LOTTIE_MODULE LOTTIE_TEST RMLUI_SAMPLES RMLUI_LUA_BINDINGS
    RMLUI_HARFBUZZ_SAMPLE RMLUI_TRACY_PROFILING RMLUI_TRACY_MEMORY_PROFILING
    RMLUI_TRACY_CONFIGURATION RMLUI_PRECOMPILED_HEADERS RMLUI_WARNINGS_AS_ERRORS
    RMLUI_COMPILER_OPTIONS)
    set(${disabled} OFF CACHE BOOL "" FORCE)
endforeach()
set(LOTTIE_THREAD ON CACHE BOOL "" FORCE)
set(LOTTIE_CACHE ON CACHE BOOL "" FORCE)
he_android_dependency(PlutoVG)
he_android_dependency(LunaSVG)
he_android_dependency(rlottie)
# The vendored script attaches its shared-library export map to static consumers.
# HGame must retain its own host exports; a static archive has no export map.
foreach(property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(rlottieLinks rlottie ${property})
    if(rlottieLinks)
        list(FILTER rlottieLinks EXCLUDE REGEX "version-script=")
        set_property(TARGET rlottie PROPERTY ${property} "${rlottieLinks}")
    endif()
endforeach()
set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
set(RMLUI_LOTTIE_PLUGIN ON CACHE BOOL "" FORCE)
set(RMLUI_SVG_PLUGIN ON CACHE BOOL "" FORCE)
he_android_dependency(RmlUi)

add_library(headers INTERFACE)
target_include_directories(headers INTERFACE "${ThirdPartyDir}"
    "${ThirdPartyDir}/entt/include" "${ThirdPartyDir}/assimp/include"
    "${ThirdPartyDir}/JoltPhysics")
add_library(libs INTERFACE)
target_link_libraries(libs INTERFACE zlibstatic)

# Some included dependency scripts change this variable in their caller scope.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
