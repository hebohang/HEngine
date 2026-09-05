include_guard(GLOBAL)

if(NOT WIN32)
    return()
endif()

# Keep the development headers and the redistributable runtime on the same
# stable Microsoft package. The middle package component is the loader-facing
# D3D12SDKVersion exported by each process executable.
set(HE_D3D12_AGILITY_PACKAGE_VERSION "1.619.5")
set(HE_D3D12_AGILITY_SDK_VERSION 619)
set(HE_D3D12_AGILITY_CORE_SHA256
    "eddf4cff4eda8162624b88694ad2adf4b09bc5aee6339191f39adf8ae48b41e7")

# Configure must be deterministic and offline: VS, Agent, codegen and every
# project build consume the same engine-owned SDK subset. An explicit CMake
# cache value wins; the legacy environment variable only seeds a new cache.
set(heAgilityDefaultRoot
    "${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/DirectXAgilitySDK/${HE_D3D12_AGILITY_PACKAGE_VERSION}")
set(heAgilityLegacyDefaultRoot
    "${CMAKE_SOURCE_DIR}/Engine/Dependencies/DirectXAgilitySDK/${HE_D3D12_AGILITY_PACKAGE_VERSION}")
if(DEFINED HE_D3D12_AGILITY_SDK_ROOT AND
   "${HE_D3D12_AGILITY_SDK_ROOT}" STREQUAL "${heAgilityLegacyDefaultRoot}")
    set(HE_D3D12_AGILITY_SDK_ROOT
        "${heAgilityDefaultRoot}"
        CACHE PATH "DirectX Agility SDK root (bundled subset or extracted NuGet package)."
        FORCE)
endif()
if(NOT DEFINED HE_D3D12_AGILITY_SDK_ROOT AND
   DEFINED ENV{HE_D3D12_AGILITY_SDK_SOURCE_DIR} AND
   NOT "$ENV{HE_D3D12_AGILITY_SDK_SOURCE_DIR}" STREQUAL "")
    file(TO_CMAKE_PATH
        "$ENV{HE_D3D12_AGILITY_SDK_SOURCE_DIR}"
        heAgilityDefaultRoot)
endif()
set(HE_D3D12_AGILITY_SDK_ROOT
    "${heAgilityDefaultRoot}"
    CACHE PATH "DirectX Agility SDK root (bundled subset or extracted NuGet package).")
set(heAgilitySdkRoot "${HE_D3D12_AGILITY_SDK_ROOT}")
get_filename_component(heAgilitySdkRoot "${heAgilitySdkRoot}" ABSOLUTE
    BASE_DIR "${CMAKE_SOURCE_DIR}")

if(IS_DIRECTORY "${heAgilitySdkRoot}/Include" AND
   IS_DIRECTORY "${heAgilitySdkRoot}/Runtime/x64")
    set(HE_D3D12_AGILITY_INCLUDE_DIR "${heAgilitySdkRoot}/Include")
    set(HE_D3D12_AGILITY_RUNTIME_DIR "${heAgilitySdkRoot}/Runtime/x64")
elseif(IS_DIRECTORY "${heAgilitySdkRoot}/build/native/include" AND
       IS_DIRECTORY "${heAgilitySdkRoot}/build/native/bin/x64")
    set(HE_D3D12_AGILITY_INCLUDE_DIR "${heAgilitySdkRoot}/build/native/include")
    set(HE_D3D12_AGILITY_RUNTIME_DIR "${heAgilitySdkRoot}/build/native/bin/x64")
else()
    message(FATAL_ERROR
        "DirectX Agility SDK root has neither the bundled Include/Runtime layout nor "
        "the official build/native layout: ${heAgilitySdkRoot}")
endif()
set(HE_D3D12_AGILITY_CORE_DLL
    "${HE_D3D12_AGILITY_RUNTIME_DIR}/D3D12Core.dll")
set(HE_D3D12_AGILITY_RUNTIME_LICENSE
    "${heAgilitySdkRoot}/LICENSE.txt")
set(HE_D3D12_AGILITY_HEADERS_LICENSE
    "${heAgilitySdkRoot}/LICENSE-CODE.txt")
set(HE_D3D12_AGILITY_PACKAGE_MANIFEST
    "${heAgilitySdkRoot}/Microsoft.Direct3D.D3D12.nuspec")
set(HE_D3D12_AGILITY_DISTRIBUTABLE_LIST
    "${heAgilitySdkRoot}/distributable files.txt")

set(heAgilityRequiredHeaderNames
    d3d12.h
    d3d12.idl
    d3d12compatibility.h
    d3d12compatibility.idl
    d3d12compiler.h
    d3d12compiler.idl
    D3D12Events.h
    d3d12markerapienums.idl
    d3d12sdklayers.h
    d3d12sdklayers.idl
    d3d12shader.h
    D3D12TokenizedProgramFormat.hpp
    d3d12video.h
    d3d12video.idl
    d3dcommon.h
    d3dcommon.idl
    d3dshadercacheregistration.h
    d3dshadercacheregistration.idl
    dxgiformat.h
    dxgiformat.idl
    d3dx12/d3dx12_barriers.h
    d3dx12/d3dx12_check_feature_support.h
    d3dx12/d3dx12_core.h
    d3dx12/d3dx12_default.h
    d3dx12/d3dx12_pipeline_state_stream.h
    d3dx12/d3dx12_property_format_table.h
    d3dx12/d3dx12_render_pass.h
    d3dx12/d3dx12_resource_helpers.h
    d3dx12/d3dx12_root_signature.h
    d3dx12/d3dx12_state_object.h
    d3dx12/d3dx12.h)
set(heAgilityRequiredFiles
    "${HE_D3D12_AGILITY_CORE_DLL}"
    "${HE_D3D12_AGILITY_RUNTIME_LICENSE}"
    "${HE_D3D12_AGILITY_HEADERS_LICENSE}"
    "${HE_D3D12_AGILITY_PACKAGE_MANIFEST}"
    "${HE_D3D12_AGILITY_DISTRIBUTABLE_LIST}")
foreach(requiredAgilityHeader IN LISTS heAgilityRequiredHeaderNames)
    list(APPEND heAgilityRequiredFiles
        "${HE_D3D12_AGILITY_INCLUDE_DIR}/${requiredAgilityHeader}")
endforeach()
foreach(requiredAgilityFile IN LISTS heAgilityRequiredFiles)
    if(NOT EXISTS "${requiredAgilityFile}" OR IS_DIRECTORY "${requiredAgilityFile}")
        message(FATAL_ERROR
            "Microsoft.Direct3D.D3D12 ${HE_D3D12_AGILITY_PACKAGE_VERSION} is incomplete: "
            "${requiredAgilityFile}")
    endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${heAgilityRequiredFiles})
file(READ "${HE_D3D12_AGILITY_PACKAGE_MANIFEST}" heAgilityPackageManifest)
if(NOT heAgilityPackageManifest MATCHES
   "<version>[ \t\r\n]*${HE_D3D12_AGILITY_PACKAGE_VERSION}[ \t\r\n]*</version>")
    message(FATAL_ERROR
        "Agility SDK source override is not Microsoft.Direct3D.D3D12 "
        "${HE_D3D12_AGILITY_PACKAGE_VERSION}")
endif()
file(READ "${HE_D3D12_AGILITY_DISTRIBUTABLE_LIST}" heAgilityDistributableList)
if(NOT heAgilityDistributableList MATCHES
   "(^|[\r\n])D3D12Core[.]dll([\r\n]|$)")
    message(FATAL_ERROR
        "DirectX Agility SDK distributable list does not permit D3D12Core.dll")
endif()
file(STRINGS "${HE_D3D12_AGILITY_INCLUDE_DIR}/d3d12.h"
    heAgilityHeaderVersionLine
    REGEX "D3D12_SDK_VERSION"
    LIMIT_COUNT 1)
if(NOT heAgilityHeaderVersionLine MATCHES
   "D3D12_SDK_VERSION[^0-9]+${HE_D3D12_AGILITY_SDK_VERSION}([^0-9]|$)")
    message(FATAL_ERROR
        "Agility SDK headers do not declare D3D12_SDK_VERSION="
        "${HE_D3D12_AGILITY_SDK_VERSION}")
endif()
file(SHA256 "${HE_D3D12_AGILITY_CORE_DLL}" heAgilityCoreSha256)
if(NOT "${heAgilityCoreSha256}" STREQUAL "${HE_D3D12_AGILITY_CORE_SHA256}")
    message(FATAL_ERROR
        "D3D12Core.dll does not match Microsoft.Direct3D.D3D12 "
        "${HE_D3D12_AGILITY_PACKAGE_VERSION}: ${heAgilityCoreSha256}")
endif()

# NVRHI uses the DirectX-Headers include spelling (<directx/d3d12.h>), while
# the official Agility NuGet package installs the matching headers at the
# include root. Small forwarding headers preserve NVRHI's public contract.
set(HE_D3D12_AGILITY_COMPAT_INCLUDE_DIR
    "${CMAKE_BINARY_DIR}/generated/DirectXAgilitySDK/include")
file(MAKE_DIRECTORY "${HE_D3D12_AGILITY_COMPAT_INCLUDE_DIR}/directx")
foreach(agilityHeader IN ITEMS
    d3d12.h
    d3d12compatibility.h
    d3d12sdklayers.h
    d3d12shader.h
    d3d12video.h
    d3dcommon.h
    dxgiformat.h
    dxgi1_6.h)
    file(WRITE
        "${HE_D3D12_AGILITY_COMPAT_INCLUDE_DIR}/directx/${agilityHeader}"
        "#pragma once\n#include <${agilityHeader}>\n")
endforeach()
file(WRITE
    "${HE_D3D12_AGILITY_COMPAT_INCLUDE_DIR}/directx/d3dx12.h"
    "#pragma once\n#include <d3dx12/d3dx12.h>\n")

if(NOT TARGET HEDirectXHeaders)
    add_library(HEDirectXHeaders INTERFACE)
    target_include_directories(HEDirectXHeaders SYSTEM INTERFACE
        "${HE_D3D12_AGILITY_COMPAT_INCLUDE_DIR}"
        "${HE_D3D12_AGILITY_INCLUDE_DIR}")
    add_library(Microsoft::DirectX-Headers ALIAS HEDirectXHeaders)
endif()

if(NOT TARGET HEDirectXGuids)
    # The Windows SDK dxguid import library supplies the GUID definitions used
    # by the current NVRHI/DX12 path. Keep the target name NVRHI expects without
    # compiling a second, mismatched DirectX-Headers snapshot.
    add_library(HEDirectXGuids INTERFACE)
    target_link_libraries(HEDirectXGuids INTERFACE dxguid)
    add_library(Microsoft::DirectX-Guids ALIAS HEDirectXGuids)
endif()

set(HE_D3D12_AGILITY_EXPORT_SOURCE
    "${CMAKE_BINARY_DIR}/generated/DirectXAgilitySDK/D3D12AgilitySDKExports.cpp")
configure_file(
    "${CMAKE_SOURCE_DIR}/CMake/D3D12AgilitySDKExports.cpp.in"
    "${HE_D3D12_AGILITY_EXPORT_SOURCE}"
    @ONLY)
file(WRITE "${CMAKE_BINARY_DIR}/DirectXAgilitySDK.version"
    "${HE_D3D12_AGILITY_SDK_VERSION}\n")
file(WRITE "${CMAKE_BINARY_DIR}/DirectXAgilitySDK.package-version"
    "${HE_D3D12_AGILITY_PACKAGE_VERSION}\n")

function(he_enable_d3d12_agility targetName)
    if(NOT TARGET "${targetName}")
        message(FATAL_ERROR "Cannot enable the Agility SDK for missing target: ${targetName}")
    endif()

    target_sources("${targetName}" PRIVATE "${HE_D3D12_AGILITY_EXPORT_SOURCE}")
    set(agilityRuntimeTarget "${targetName}AgilityRuntime")
    add_custom_target("${agilityRuntimeTarget}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:${targetName}>/D3D12"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${HE_D3D12_AGILITY_CORE_DLL}"
            "$<TARGET_FILE_DIR:${targetName}>/D3D12/D3D12Core.dll"
        DEPENDS "${HE_D3D12_AGILITY_CORE_DLL}"
        COMMENT
            "Installing DirectX 12 Agility SDK ${HE_D3D12_AGILITY_PACKAGE_VERSION} for ${targetName}"
        VERBATIM)
    set_target_properties("${agilityRuntimeTarget}" PROPERTIES
        FOLDER "ThirdParty/DirectXAgilitySDK")
    add_dependencies("${targetName}" "${agilityRuntimeTarget}")
endfunction()

message(STATUS
    "DirectX 12 Agility SDK ${HE_D3D12_AGILITY_PACKAGE_VERSION} "
    "(D3D12SDKVersion=${HE_D3D12_AGILITY_SDK_VERSION})")
