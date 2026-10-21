set(EngineRootDir "${RootDir}/Engine")
set(EngineContentDir "${EngineRootDir}/Content")
set(EngineGeneratedContentDir "${CMAKE_BINARY_DIR}/GeneratedContent/Engine")
set(EngineResourcesDir "${EngineRootDir}/Resources")
set(EngineSourceDir "${EngineRootDir}/Source")
set(EngineBinariesDir "${EngineRootDir}/Binaries")
if(ANDROID OR APPLE)
    set(BINARY_ROOT_DIR "${CMAKE_BINARY_DIR}/bin")
endif()

get_filename_component(ProjectSelection "${HE_PROJECT_DIR}" ABSOLUTE BASE_DIR "${RootDir}")
if(IS_DIRECTORY "${ProjectSelection}")
    set(ProjectRootDir "${ProjectSelection}")
elseif(EXISTS "${ProjectSelection}")
    get_filename_component(ProjectSelectionName "${ProjectSelection}" NAME)
    if(NOT ProjectSelectionName STREQUAL "Project.heproject")
        message(FATAL_ERROR "HE_PROJECT_DIR file must be named Project.heproject: ${ProjectSelection}")
    endif()
    get_filename_component(ProjectRootDir "${ProjectSelection}" DIRECTORY)
else()
    message(FATAL_ERROR "HE_PROJECT_DIR does not exist: ${ProjectSelection}")
endif()

set(ProjectDescriptorFile "${ProjectRootDir}/Project.heproject")
if(NOT EXISTS "${ProjectDescriptorFile}")
    message(FATAL_ERROR
        "HE_PROJECT_DIR does not contain Project.heproject: ${ProjectRootDir}")
endif()

if(PACK_GAME AND MSVC)
    # Project sources may live outside the Engine checkout. Map every physical
    # compilation root so __FILE__/source-location strings in the shipping EXE
    # are deterministic and cannot disclose the developer's directory layout.
    file(TO_NATIVE_PATH "${RootDir}" HE_SOURCE_NATIVE_PATH)
    file(TO_NATIVE_PATH "${ProjectRootDir}" HE_PROJECT_NATIVE_PATH)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" HE_BUILD_NATIVE_PATH)
    add_compile_options(
        "/experimental:deterministic"
        "/pathmap:${HE_PROJECT_NATIVE_PATH}=Project"
        "/pathmap:${HE_BUILD_NATIVE_PATH}=Build"
        "/pathmap:${HE_SOURCE_NATIVE_PATH}=."
        # Limit cl.exe fan-out so the large runtime PCH remains buildable on
        # machines with a modest page file.
        "/MP4")
endif()

file(READ "${ProjectDescriptorFile}" ProjectDescriptorJson)
string(JSON HE_PROJECT_SCHEMA_VERSION ERROR_VARIABLE ProjectSchemaError
    GET "${ProjectDescriptorJson}" SchemaVersion)
string(JSON HE_PROJECT_ID ERROR_VARIABLE ProjectIdError
    GET "${ProjectDescriptorJson}" ProjectId)
string(JSON HE_PROJECT_NAME ERROR_VARIABLE ProjectNameError
    GET "${ProjectDescriptorJson}" DisplayName)
string(JSON HE_PROJECT_ENGINE_VERSION ERROR_VARIABLE ProjectEngineVersionError
    GET "${ProjectDescriptorJson}" EngineVersion)
string(JSON HE_PROJECT_EXECUTABLE_NAME ERROR_VARIABLE ProjectExecutableNameError
    GET "${ProjectDescriptorJson}" ExecutableName)
string(JSON HE_PROJECT_GAME_CONFIG ERROR_VARIABLE ProjectGameConfigError
    GET "${ProjectDescriptorJson}" GameConfig)
set(HE_PACKAGE_GAME_CONFIG "" CACHE STRING "Optional project-relative shipping game configuration")
if(NOT HE_PACKAGE_GAME_CONFIG STREQUAL "")
    if(NOT PACK_GAME)
        message(FATAL_ERROR "HE_PACKAGE_GAME_CONFIG is only supported for PACK_GAME builds")
    endif()
    set(HE_PROJECT_GAME_CONFIG "${HE_PACKAGE_GAME_CONFIG}")
endif()
string(JSON HE_PROJECT_PACKAGE_CONFIG ERROR_VARIABLE ProjectPackageConfigError
    GET "${ProjectDescriptorJson}" PackageConfig)
string(JSON HE_PROJECT_LEGACY_SAVED_DIRECTORY ERROR_VARIABLE ProjectLegacySavedDirectoryError
    GET "${ProjectDescriptorJson}" LegacySavedDirectory)
if(ProjectLegacySavedDirectoryError)
    set(HE_PROJECT_LEGACY_SAVED_DIRECTORY "")
else()
    string(TOUPPER "${HE_PROJECT_LEGACY_SAVED_DIRECTORY}" ProjectLegacySavedDirectoryUpper)
    if(NOT HE_PROJECT_LEGACY_SAVED_DIRECTORY MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$" OR
       HE_PROJECT_LEGACY_SAVED_DIRECTORY MATCHES "[.]$" OR
       ProjectLegacySavedDirectoryUpper MATCHES "^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])([.].*)?$")
        message(FATAL_ERROR "Project.heproject LegacySavedDirectory must be a safe directory name")
    endif()
endif()
if(ProjectSchemaError OR NOT HE_PROJECT_SCHEMA_VERSION EQUAL 1)
    message(FATAL_ERROR "Project.heproject SchemaVersion must be 1")
endif()
string(LENGTH "${HE_PROJECT_ID}" ProjectIdLength)
string(TOUPPER "${HE_PROJECT_ID}" ProjectIdUpper)
if(ProjectIdError OR ProjectIdLength GREATER 128 OR
   NOT HE_PROJECT_ID MATCHES "^[a-z0-9][a-z0-9._-]*$" OR
   HE_PROJECT_ID MATCHES "[.]$" OR
   ProjectIdUpper MATCHES "^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])([.].*)?$")
    message(FATAL_ERROR
        "Project.heproject ProjectId must use lowercase ASCII letters, digits, '.', '_' or '-'")
endif()
string(LENGTH "${HE_PROJECT_NAME}" ProjectNameLength)
if(ProjectNameError OR ProjectNameLength EQUAL 0 OR
   ProjectNameLength GREATER 128 OR HE_PROJECT_NAME MATCHES "[\n\r]")
    message(FATAL_ERROR "Project.heproject must contain a non-empty DisplayName string")
endif()
if(ProjectEngineVersionError OR NOT HE_PROJECT_ENGINE_VERSION STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR
        "Project.heproject EngineVersion '${HE_PROJECT_ENGINE_VERSION}' does not match HEngine ${PROJECT_VERSION}")
endif()
string(LENGTH "${HE_PROJECT_EXECUTABLE_NAME}" ProjectExecutableNameLength)
string(TOUPPER "${HE_PROJECT_EXECUTABLE_NAME}" ProjectExecutableNameUpper)
if(ProjectExecutableNameError OR ProjectExecutableNameLength GREATER 64 OR
   NOT HE_PROJECT_EXECUTABLE_NAME MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$" OR
   HE_PROJECT_EXECUTABLE_NAME MATCHES "[.]$" OR
   HE_PROJECT_EXECUTABLE_NAME MATCHES "[.][Ee][Xx][Ee]$" OR
   ProjectExecutableNameUpper MATCHES "^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])([.].*)?$")
    message(FATAL_ERROR
        "Project.heproject ExecutableName must contain only ASCII letters, digits, '.', '_' or '-'")
endif()
string(REPLACE "\\" "/" HE_PROJECT_GAME_CONFIG "${HE_PROJECT_GAME_CONFIG}")
if(ProjectGameConfigError OR IS_ABSOLUTE "${HE_PROJECT_GAME_CONFIG}" OR
   HE_PROJECT_GAME_CONFIG MATCHES "(^|/)\\.\\.(/|$)" OR
   NOT HE_PROJECT_GAME_CONFIG MATCHES "^[A-Za-z0-9_./-]+[.]json$")
    message(FATAL_ERROR "Project.heproject GameConfig must be a project-relative path")
endif()
string(REPLACE "\\" "/" HE_PROJECT_PACKAGE_CONFIG "${HE_PROJECT_PACKAGE_CONFIG}")
if(ProjectPackageConfigError OR IS_ABSOLUTE "${HE_PROJECT_PACKAGE_CONFIG}" OR
   HE_PROJECT_PACKAGE_CONFIG MATCHES "(^|/)\\.\\.(/|$)" OR
   NOT HE_PROJECT_PACKAGE_CONFIG MATCHES "^[A-Za-z0-9_./-]+[.]json$")
    message(FATAL_ERROR "Project.heproject PackageConfig must be a project-relative path")
endif()

set(ProjectAssetsDir "${ProjectRootDir}/Assets")
set(ProjectConfigDir "${ProjectRootDir}/Config")
set(ProjectGameConfigFile "${ProjectRootDir}/${HE_PROJECT_GAME_CONFIG}")
set(ProjectPackageConfigFile "${ProjectRootDir}/${HE_PROJECT_PACKAGE_CONFIG}")
set(ProjectShadersDir "${ProjectRootDir}/Shaders")
set(ProjectScriptsDir "${ProjectRootDir}/Scripts")

set(EngineContentAssetsDir "${EngineContentDir}/Assets")
set(EngineContentShadersDir "${EngineContentDir}/Shaders")
set(EngineContentScriptsDir "${EngineContentDir}/Scripts")
set(EngineGeneratedShadersDir "${EngineGeneratedContentDir}/Shaders")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${ProjectDescriptorFile}"
    "${ProjectPackageConfigFile}"
    "${EngineContentDir}/Config/DefaultEngine.json"
    "${ProjectConfigDir}/Plugins.json")
file(MAKE_DIRECTORY "${EngineGeneratedContentDir}")

function(he_escape_cpp_string inputValue outputVariable)
    set(escapedValue "${inputValue}")
    string(REPLACE "\\" "\\\\" escapedValue "${escapedValue}")
    string(REPLACE "\"" "\\\"" escapedValue "${escapedValue}")
    string(REPLACE "\t" "\\t" escapedValue "${escapedValue}")
    string(REPLACE "\n" "\\n" escapedValue "${escapedValue}")
    string(REPLACE "\r" "\\r" escapedValue "${escapedValue}")
    set(${outputVariable} "${escapedValue}" PARENT_SCOPE)
endfunction()

if(PACK_GAME)
    set(HE_BUILD_DEFAULT_PROJECT_DIR ".")
    set(HE_BUILD_ENGINE_CONTENT_DIR ".")
    set(HE_BUILD_ENGINE_GENERATED_CONTENT_DIR ".")
elseif(HE_PORTABLE_EDITOR OR (HE_PORTABLE_PLAYER AND NOT HE_PORTABLE_PLAYER_SOURCE_CONTENT))
    set(HE_BUILD_DEFAULT_PROJECT_DIR "Project")
    set(HE_BUILD_ENGINE_CONTENT_DIR "Engine/Content")
    set(HE_BUILD_ENGINE_GENERATED_CONTENT_DIR "Engine/Content")
else()
    set(HE_BUILD_DEFAULT_PROJECT_DIR "${ProjectRootDir}")
    set(HE_BUILD_ENGINE_CONTENT_DIR "${EngineContentDir}")
    set(HE_BUILD_ENGINE_GENERATED_CONTENT_DIR "${EngineGeneratedContentDir}")
endif()

he_escape_cpp_string("${HE_BUILD_DEFAULT_PROJECT_DIR}" HE_DEFAULT_PROJECT_DIR_CPP)
he_escape_cpp_string("${HE_BUILD_ENGINE_CONTENT_DIR}" HE_ENGINE_CONTENT_DIR_CPP)
he_escape_cpp_string("${HE_BUILD_ENGINE_GENERATED_CONTENT_DIR}" HE_ENGINE_GENERATED_CONTENT_DIR_CPP)
he_escape_cpp_string("${HE_PROJECT_GAME_CONFIG}" HE_PROJECT_GAME_CONFIG_CPP)
he_escape_cpp_string("${HE_PROJECT_NAME}" HE_PROJECT_DISPLAY_NAME_CPP)
he_escape_cpp_string("${HE_PROJECT_ENGINE_VERSION}" HE_PROJECT_ENGINE_VERSION_CPP)
he_escape_cpp_string("${HE_PROJECT_EXECUTABLE_NAME}" HE_PROJECT_EXECUTABLE_NAME_CPP)
he_escape_cpp_string("${HE_PROJECT_ID}" HE_PROJECT_ID_CPP)
he_escape_cpp_string("${HE_PROJECT_LEGACY_SAVED_DIRECTORY}" HE_PROJECT_LEGACY_SAVED_DIRECTORY_CPP)
configure_file(
    "${RootDir}/CMake/ProjectBuildConfig.h.in"
    "${HE_ENGINE_GENERATED_INCLUDE_DIR}/HEngine/ProjectBuildConfig.h"
    @ONLY)

foreach(requiredDirectory IN ITEMS
    "${EngineContentDir}"
    "${ProjectAssetsDir}"
    "${ProjectConfigDir}")
    if(NOT IS_DIRECTORY "${requiredDirectory}")
        message(FATAL_ERROR "Required engine/project directory is missing: ${requiredDirectory}")
    endif()
endforeach()
if(NOT EXISTS "${ProjectGameConfigFile}")
    message(FATAL_ERROR "Project GameConfig does not exist: ${ProjectGameConfigFile}")
endif()
if(NOT EXISTS "${ProjectPackageConfigFile}")
    message(FATAL_ERROR "Project PackageConfig does not exist: ${ProjectPackageConfigFile}")
endif()
if(NOT EXISTS "${EngineContentDir}/Config/EngineCook.json")
    message(FATAL_ERROR "Engine cook config is missing from Engine/Content")
endif()

if (WIN32)
    set(HE_RUNTIME_OUTPUT_DIR "" CACHE PATH
        "Optional isolated runtime output directory for command-line builds.")
    if(HE_RUNTIME_OUTPUT_DIR)
        get_filename_component(BINARY_ROOT_DIR "${HE_RUNTIME_OUTPUT_DIR}" ABSOLUTE BASE_DIR "${RootDir}")
    elseif(PACK_GAME OR NOT ProjectRootDir STREQUAL "${RootDir}/Project")
        # Shipping and external-project graphs must never link or copy tools
        # into the shared Engine checkout.
        set(BINARY_ROOT_DIR "${CMAKE_BINARY_DIR}/bin")
    else()
        set(BINARY_ROOT_DIR "${EngineBinariesDir}/Win64")
    endif()
    set(CMAKE_INSTALL_PREFIX "${BINARY_ROOT_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${BINARY_ROOT_DIR}")
endif()

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "HEngine only supports 64-bit platforms now")
endif ()

if (PROJECT_BINARY_DIR STREQUAL PROJECT_SOURCE_DIR)
    message(WARNING "The binary directory of CMake cannot be the same as source directory!")
endif()

if (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration." FORCE)
endif()

if (WIN32)
    add_definitions(-DNOMINMAX -D_USE_MATH_DEFINES)

    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        link_directories(
            "$ENV{VCToolsInstallDir}/ATLMFC/lib/x64"
            "$ENV{VCToolsInstallDir}/lib/x64"
            "$ENV{UniversalCRTSdkDir}/Lib/$ENV{UCRTVersion}/ucrt/x64"
            "$ENV{WindowsSdkDir}/Lib/$ENV{WindowsSDKVersion}/um/x64"
        )
    endif()
endif()

# Ninja owns parallel compilation, so clang-cl needs no compiler fan-out flag.
# MSVC keeps the repository's existing diagnostics and /MP behavior.
if("${CMAKE_CXX_COMPILER_ID}" MATCHES "MSVC")
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /D _CRT_SECURE_NO_WARNINGS /MP /utf-8")
    # add_compile_options("/MP")
endif()

if (NOT MSVC)
    find_program(CCACHE_PROGRAM ccache)
    if (CCACHE_PROGRAM)
        message(STATUS "Found CCache: ${CCACHE_PROGRAM}")
        set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ${CCACHE_PROGRAM})
        set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ${CCACHE_PROGRAM})
    endif()
endif()

add_subdirectory(Source)
