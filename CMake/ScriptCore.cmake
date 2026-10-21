# This target intentionally has no transitive engine, Python or .NET dependency.
# It may also be included by isolated dependency-boundary checks.
if(NOT TARGET HEngineScriptCore)
    get_filename_component(heScriptRoot "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    set(heScriptApiDir "${CMAKE_CURRENT_BINARY_DIR}/scriptcore-api")
    add_custom_command(OUTPUT "${heScriptApiDir}/ScriptApi.h"
        COMMAND "${CMAKE_COMMAND}" "-DOUTPUT_DIR=${heScriptApiDir}" -P "${heScriptRoot}/Tools/Scripting/GenerateApi.cmake"
        DEPENDS "${heScriptRoot}/Tools/Scripting/Api.schema.json" "${heScriptRoot}/Tools/Scripting/GenerateApi.cmake"
        VERBATIM)
    add_library(HEngineScriptCore STATIC
        "${heScriptApiDir}/ScriptApi.h"
        "${heScriptRoot}/Engine/Source/Runtime/Scripting/ScriptBackendRegistry.cpp"
        "${heScriptRoot}/Engine/Source/Runtime/Scripting/ScriptCore.cpp"
        "${heScriptRoot}/Engine/Source/Runtime/Scripting/ScriptFieldsJson.cpp")
    target_sources(HEngineScriptCore PRIVATE "${heScriptRoot}/Engine/Source/Runtime/Scripting/ScriptAggregates.cpp")
    target_compile_features(HEngineScriptCore PUBLIC cxx_std_20)
    target_include_directories(HEngineScriptCore PUBLIC "${heScriptRoot}/Engine/Source" "${heScriptApiDir}"
        PRIVATE "${heScriptRoot}/Engine/Source/ThirdParty/json/include")
endif()
