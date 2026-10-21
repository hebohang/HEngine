ADD_PLUGIN(CSharpScript)
if(ANDROID)
    PLUGIN_ADD_SOURCE(CSharpPluginEntry.cpp CSharpBackend.h CSharpBackend.cpp)
    target_link_libraries(CSharpScript PRIVATE HEngineRuntime)
    END_PLUGIN()
    return()
endif()
if(HE_SCRIPT_CSHARP_WASM)
    PLUGIN_ADD_SOURCE(CSharpPluginEntry.cpp CSharpBackend.h CSharpBackend.cpp)
    target_sources(CSharpScript PRIVATE "${EngineRuntimeDir}/Platform/Scripting/WasmCSharpHost.cpp")
    target_link_libraries(CSharpScript PRIVATE HEnginePortableOptions)
    END_PLUGIN()
    return()
endif()
find_program(HE_DOTNET_EXECUTABLE NAMES dotnet REQUIRED)
find_program(HE_SCRIPT_POWERSHELL NAMES powershell.exe pwsh REQUIRED)
get_filename_component(HE_DOTNET_ROOT "${HE_DOTNET_EXECUTABLE}" DIRECTORY)
if(APPLE)
    # BuildManaged stages the pinned arm64 runtime for Editor and Player alike.
    set(HE_CSHARP_HOSTFXR "${BINARY_ROOT_DIR}/Scripting/CSharp/Runtime/host/fxr/10.0.10/libhostfxr.dylib")
elseif(HE_WINDOWS_ARCH STREQUAL "arm64")
    set(HE_CSHARP_HOSTFXR "${HE_ARM64_RUNTIME_ROOT}/host/fxr/10.0.10/hostfxr.dll")
else()
    set(HE_CSHARP_HOSTFXR "${HE_DOTNET_ROOT}/host/fxr/10.0.10/hostfxr.dll")
endif()
if(NOT APPLE AND NOT PACK_GAME AND NOT EXISTS "${HE_CSHARP_HOSTFXR}")
    message(FATAL_ERROR "C# plugin requires .NET runtime 10.0.10: ${HE_CSHARP_HOSTFXR}")
endif()
if(PACK_GAME)
    set(HE_CSHARP_HOSTFXR "")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/CSharpBuildConfig.h"
    "#pragma once\n#define HE_CSHARP_DEVELOPMENT_HOSTFXR R\"(${HE_CSHARP_HOSTFXR})\"\n#if WITH_EDITOR\n#define HE_CSHARP_EDITOR_POWERSHELL R\"(${HE_SCRIPT_POWERSHELL})\"\n#define HE_CSHARP_EDITOR_SCRIPT R\"(${CMAKE_SOURCE_DIR}/Tools/Scripting/CompileEditor.ps1)\"\n#define HE_CSHARP_EDITOR_DOTNET R\"(${HE_DOTNET_EXECUTABLE})\"\n#define HE_CSHARP_EDITOR_CMAKE R\"(${CMAKE_COMMAND})\"\n#endif\n")
set(HE_CSHARP_OUTPUT "${BINARY_ROOT_DIR}/Scripting/CSharp")
set(HE_CSHARP_WORK "${CMAKE_CURRENT_BINARY_DIR}/Managed")
set(HE_SCRIPT_QUALITY "Desktop" CACHE STRING "Managed presentation quality: Desktop or Portable")
if(NOT HE_SCRIPT_QUALITY MATCHES "^(Desktop|Portable)$")
    message(FATAL_ERROR "Unsupported managed presentation quality: ${HE_SCRIPT_QUALITY}")
endif()
set(HE_CSHARP_SHIPPING_ARGS "")
set(HE_CSHARP_PLATFORM_INPUTS "")
if(APPLE)
    list(APPEND HE_CSHARP_SHIPPING_ARGS -Platform macOS)
    list(APPEND HE_CSHARP_PLATFORM_INPUTS
        "${CMAKE_SOURCE_DIR}/Tools/Scripting/runtime.macos-arm64.lock.json"
        "${CMAKE_SOURCE_DIR}/Tools/macOS/PinnedRuntime.py")
endif()
if(PACK_GAME)
    list(APPEND HE_CSHARP_SHIPPING_ARGS -Shipping)
else()
    list(APPEND HE_CSHARP_SHIPPING_ARGS -EditorHost)
endif()
file(GLOB_RECURSE HE_CSHARP_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Engine/Managed/*.cs"
    "${CMAKE_SOURCE_DIR}/Engine/Managed/*.csproj"
    "${CMAKE_SOURCE_DIR}/Engine/Managed/*.props"
    "${CMAKE_SOURCE_DIR}/Engine/Managed/*lock.json"
    "${CMAKE_SOURCE_DIR}/Engine/Managed/global.json"
    "${ProjectRootDir}/Scripts/CSharp/*.cs"
    "${ProjectRootDir}/Scripts/CSharp/*.csproj"
    "${ProjectRootDir}/Scripts/CSharp/*lock.json"
    "${ProjectRootDir}/Scripts/CSharp/global.json"
    "${ProjectRootDir}/Scripts/*.hemodule"
    "${ProjectRootDir}/Scripts/*.cs"
    "${CMAKE_SOURCE_DIR}/Tools/Scripting/RegistryGenerator/*.cs"
    "${CMAKE_SOURCE_DIR}/Tools/Scripting/RegistryGenerator/*.csproj")
list(APPEND HE_CSHARP_INPUTS "${CMAKE_SOURCE_DIR}/Tools/Scripting/RegistryGenerator/packages.lock.json")
list(FILTER HE_CSHARP_INPUTS EXCLUDE REGEX "/(bin|obj)/")
# Projects can declare resource-generation inputs without engine knowledge of game assets.
set(HE_MANAGED_INPUT_MANIFEST "${ProjectRootDir}/Config/ManagedInputs.json")
if(EXISTS "${HE_MANAGED_INPUT_MANIFEST}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HE_MANAGED_INPUT_MANIFEST}")
    list(APPEND HE_CSHARP_INPUTS "${HE_MANAGED_INPUT_MANIFEST}")
    file(READ "${HE_MANAGED_INPUT_MANIFEST}" HE_MANAGED_INPUT_JSON)
    string(JSON HE_MANAGED_INPUT_COUNT LENGTH "${HE_MANAGED_INPUT_JSON}" inputs)
    if(HE_MANAGED_INPUT_COUNT GREATER 0)
        math(EXPR HE_MANAGED_INPUT_LAST "${HE_MANAGED_INPUT_COUNT} - 1")
        foreach(HE_MANAGED_INPUT_INDEX RANGE ${HE_MANAGED_INPUT_LAST})
            string(JSON HE_MANAGED_INPUT_PATTERN GET "${HE_MANAGED_INPUT_JSON}" inputs ${HE_MANAGED_INPUT_INDEX})
            if(IS_ABSOLUTE "${HE_MANAGED_INPUT_PATTERN}" OR HE_MANAGED_INPUT_PATTERN MATCHES "(^|[/\\\\])[.][.]([/\\\\]|$)")
                message(FATAL_ERROR "Managed build inputs must stay inside the project")
            endif()
            file(GLOB_RECURSE HE_MANAGED_EXTRA_INPUTS CONFIGURE_DEPENDS "${ProjectRootDir}/${HE_MANAGED_INPUT_PATTERN}")
            list(APPEND HE_CSHARP_INPUTS ${HE_MANAGED_EXTRA_INPUTS})
        endforeach()
    endif()
endif()
add_custom_command(OUTPUT "${HE_CSHARP_WORK}/build.stamp"
    COMMAND "${HE_SCRIPT_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/Tools/Scripting/BuildManaged.ps1"
        -Project "${ProjectRootDir}" -WorkDirectory "${HE_CSHARP_WORK}"
        -OutputDirectory "${HE_CSHARP_OUTPUT}" -Configuration "$<IF:$<CONFIG:Debug>,Debug,Release>"
        -Dotnet "${HE_DOTNET_EXECUTABLE}" -CMake "${CMAKE_COMMAND}" -Quality "${HE_SCRIPT_QUALITY}" ${HE_CSHARP_SHIPPING_ARGS}
    COMMAND "${CMAKE_COMMAND}" -E touch "${HE_CSHARP_WORK}/build.stamp"
    DEPENDS ${HE_CSHARP_INPUTS}
        "${CMAKE_SOURCE_DIR}/Tools/Scripting/BuildManaged.ps1"
        "${CMAKE_SOURCE_DIR}/Tools/Scripting/Modules.py"
        "${CMAKE_SOURCE_DIR}/Tools/Scripting/MergeModuleRegistries.py"
        "${CMAKE_SOURCE_DIR}/Tools/Scripting/GetPinnedRuntime.ps1"
        "${CMAKE_SOURCE_DIR}/Tools/Scripting/runtime.lock.json"
        ${HE_CSHARP_PLATFORM_INPUTS}
        "${CMAKE_SOURCE_DIR}/Tools/Scripting/Api.schema.json"
        "${CMAKE_SOURCE_DIR}/Tools/Scripting/GenerateApi.cmake"
    BYPRODUCTS "${HE_CSHARP_OUTPUT}/HEngine.ScriptRuntime.dll"
        "${HE_CSHARP_OUTPUT}/HEngine.ScriptCore.dll" "${HE_CSHARP_OUTPUT}/GameScripts.dll"
        "${HE_CSHARP_OUTPUT}/GameScripts.metadata.json" "${HE_CSHARP_OUTPUT}/build.json"
        "${HE_CSHARP_OUTPUT}/HEngine.ScriptRuntime.runtimeconfig.json"
    VERBATIM)
add_custom_target(HEngineCSharpManaged DEPENDS "${HE_CSHARP_WORK}/build.stamp")
add_dependencies(CSharpScript HEngineCSharpManaged)
PLUGIN_ADD_SOURCE(CSharpPluginEntry.cpp CSharpBackend.h CSharpBackend.cpp)
target_sources(CSharpScript PRIVATE "${EngineRuntimeDir}/Platform/Scripting/CoreClrHost.cpp")
target_include_directories(CSharpScript PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(CSharpScript PRIVATE HEngineRuntime)
if(PACK_GAME)
    target_compile_definitions(CSharpScript PRIVATE HE_PACKAGED_GAME=1)
endif()
END_PLUGIN()
