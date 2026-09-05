cmake_minimum_required(VERSION 3.20)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_EXTENSIONS OFF)

project(HEngine VERSION 1.0.0 LANGUAGES C CXX)

# Engine-owned find modules provide reproducible graphics SDK targets to
# vendored consumers without inheriting arbitrary machine SDK installations.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/CMake")

set(HE_ENGINE_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${HE_ENGINE_GENERATED_INCLUDE_DIR}/HEngine")
configure_file(
    "${CMAKE_SOURCE_DIR}/CMake/EngineVersion.h.in"
    "${HE_ENGINE_GENERATED_INCLUDE_DIR}/HEngine/EngineVersion.h"
    @ONLY)

option(PACK_GAME "Build the shipping game and cooked package." OFF)
option(HE_BUILD_DEV_GAME "Build the game against uncooked project content without the editor." OFF)
option(HE_PORTABLE_EDITOR "Build HEngineEditor with executable-relative content roots." OFF)
option(HE_ENABLE_GM "Build reusable GM developer tooling." OFF)
option(HE_BUILD_TESTS "Build the isolated headless HEngineTests consumer host." OFF)
option(HE_CODEGEN_TOOL_ONLY "Configure only the standalone HEngineParser tool." OFF)
option(CPP_CODE_GEN "Deprecated compatibility switch; codegen is a build-graph target." OFF)
set(HE_GENERATED_SOURCE_DIR "${CMAKE_BINARY_DIR}/GeneratedSource" CACHE PATH
    "Build-local root for reflection and serializer generated sources.")
get_filename_component(HE_GENERATED_SOURCE_DIR "${HE_GENERATED_SOURCE_DIR}"
    ABSOLUTE BASE_DIR "${CMAKE_BINARY_DIR}")
set(HE_GENERATED_SOURCE_BASE_DIR "${HE_GENERATED_SOURCE_DIR}")
if(CMAKE_CONFIGURATION_TYPES)
    set(HE_GENERATED_SOURCE_DIR "${HE_GENERATED_SOURCE_BASE_DIR}/$<CONFIG>")
endif()
set(GeneratedFileDir "${HE_GENERATED_SOURCE_DIR}/_generated")
set(GeneratedSeriFileDir "${GeneratedFileDir}/serializer")

set(HE_CODEGEN_PARSER_EXECUTABLE "" CACHE FILEPATH
    "Optional shared HEngineParser executable used by the codegen build node.")
set(HE_CODEGEN_HOST_OVERRIDE "" CACHE STRING
    "Optional codegen consumer host: Editor, DevGame, Tests, or Package.")
set_property(CACHE HE_CODEGEN_HOST_OVERRIDE PROPERTY STRINGS
    "" Editor DevGame Tests Package)

if(HE_CODEGEN_TOOL_ONLY)
    add_subdirectory(Engine/Source/Programs/HEngineParser)
    return()
endif()

if(PACK_GAME AND HE_BUILD_DEV_GAME)
    message(FATAL_ERROR "PACK_GAME and HE_BUILD_DEV_GAME are mutually exclusive build modes.")
endif()
if(HE_PORTABLE_EDITOR AND (PACK_GAME OR HE_BUILD_DEV_GAME OR HE_BUILD_TESTS))
    message(FATAL_ERROR
        "HE_PORTABLE_EDITOR requires the standalone Editor build mode.")
endif()
if(PACK_GAME AND HE_ENABLE_GM)
    message(FATAL_ERROR "PACK_GAME and HE_ENABLE_GM are mutually exclusive; GM tooling must not enter shipping builds.")
endif()
if(HE_BUILD_TESTS AND (NOT HE_BUILD_DEV_GAME OR NOT HE_ENABLE_GM OR PACK_GAME))
    message(FATAL_ERROR
        "HE_BUILD_TESTS requires HE_BUILD_DEV_GAME=ON, HE_ENABLE_GM=ON, and PACK_GAME=OFF.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(RootDir "${CMAKE_CURRENT_SOURCE_DIR}")

set(HE_PROJECT_DIR "${RootDir}/Project" CACHE PATH
    "HEngine project directory or Project.heproject descriptor.")

if(PACK_GAME)
    # A standalone package must not inherit optional DLL dependencies from an
    # existing developer cache. Keep all third-party libraries static.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build packaged-game dependencies statically." FORCE)
    set(GLFW_LIBRARY_TYPE "STATIC" CACHE STRING "Build GLFW statically for packaged games." FORCE)
endif()

add_subdirectory(Engine)
