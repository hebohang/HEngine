include("${CMAKE_SOURCE_DIR}/CMake/ScriptCore.cmake")
set(HE_SCRIPT_API_DIR "${CMAKE_BINARY_DIR}/generated/scripting")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DOUTPUT_DIR=${HE_SCRIPT_API_DIR}"
    -P "${CMAKE_SOURCE_DIR}/Tools/Scripting/GenerateApi.cmake"
    COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Tools/Scripting/Api.schema.json"
    "${CMAKE_SOURCE_DIR}/Tools/Scripting/GenerateApi.cmake")

RUNTIME_ADD_SOURCE(
    ScriptCore.h ScriptFieldsJson.h
    ScriptBackendRegistry.h ScriptWorld.h ScriptWorld.cpp
    ScriptEngineApi.h ScriptEngineApi.cpp
    ScriptEngine.h ScriptEngine.cpp
    SurvivorAgentBatch.h SurvivorAgentBatch.cpp
    SurvivorPickupBatch.h SurvivorPickupBatch.cpp
    SurvivorSpatialIndex.h SurvivorSpatialIndex.cpp
    SurvivorTargetingBatch.h SurvivorTargetingBatch.cpp
)
