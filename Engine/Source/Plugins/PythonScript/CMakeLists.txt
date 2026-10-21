ADD_PLUGIN(PythonScript)

PLUGIN_ADD_SOURCE(PythonPluginEntry.cpp PythonRuntime.h PythonRuntime.cpp
    PythonBackend.h PythonBackend.cpp Binding/ScriptApiBinding.h Binding/ScriptApiBinding.cpp
    Binding/ReflectedComponents.h Binding/ReflectedComponents.cpp Binding/PythonModule.cpp)
target_include_directories(PythonScript PRIVATE
    "${EngineRuntimeDir}"
    "${ProjectRootDir}/Source"
    "${HE_ENGINE_GENERATED_INCLUDE_DIR}"
    "${ThirdPartyDir}/pybind11/include")
target_link_libraries(PythonScript PRIVATE HEngineRuntime)
if(APPLE)
    find_package(Python3 3.11 EXACT REQUIRED COMPONENTS Interpreter Development.Embed)
    target_link_libraries(PythonScript PRIVATE Python3::Python)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import sys; print(sys.base_prefix)"
        OUTPUT_VARIABLE HE_PYTHON_HOME OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/PythonBuildConfig.h"
        "#pragma once\n#define HE_MACOS_PYTHON_HOME R\"(${HE_PYTHON_HOME})\"\n")
    target_include_directories(PythonScript PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    list(GET Python3_LIBRARIES 0 HE_PYTHON_LIBRARY)
    set(HE_PYTHON_EMBED_JSON "{}")
    foreach(key home stdlib library)
        if(key STREQUAL "home")
            set(value "${HE_PYTHON_HOME}")
        elseif(key STREQUAL "stdlib")
            set(value "${Python3_STDLIB}")
        else()
            set(value "${HE_PYTHON_LIBRARY}")
        endif()
        string(REPLACE "\\" "\\\\" value "${value}")
        string(REPLACE "\"" "\\\"" value "${value}")
        string(JSON HE_PYTHON_EMBED_JSON SET "${HE_PYTHON_EMBED_JSON}" "${key}" "\"${value}\"")
    endforeach()
    file(WRITE "${BINARY_ROOT_DIR}/PythonEmbed.json" "${HE_PYTHON_EMBED_JSON}\n")
else()
    if(HE_WINDOWS_ARCH STREQUAL "arm64")
        set(hePythonInclude "${HE_ARM64_PYTHON_ROOT}/include")
        set(hePythonLibrary "${HE_ARM64_PYTHON_ROOT}/libs/python311.lib")
        set(hePythonRuntime "${HE_ARM64_PYTHON_ROOT}/python311.dll")
    else()
        set(hePythonInclude "${ThirdPartyDir}/Python/include")
        set(hePythonLibrary "${ThirdPartyDir}/Python/libs/python311.lib")
        set(hePythonRuntime "${ThirdPartyDir}/Python/dlls/python311.dll")
    endif()
    target_include_directories(PythonScript PRIVATE "${hePythonInclude}")
    target_link_libraries(PythonScript PRIVATE "${hePythonLibrary}")
endif()
if(HE_ENABLE_GM)
    target_sources(PythonScript PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Binding/GMBinding.cpp")
endif()
if(PACK_GAME)
    target_compile_definitions(PythonScript PRIVATE HE_PACKAGED_GAME=1)
endif()
if(MSVC)
    target_compile_options(PythonScript PRIVATE /bigobj)
endif()
if(WIN32)
add_custom_command(TARGET PythonScript POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${hePythonRuntime}" "${BINARY_ROOT_DIR}/python311.dll"
    VERBATIM)
endif()

END_PLUGIN()
