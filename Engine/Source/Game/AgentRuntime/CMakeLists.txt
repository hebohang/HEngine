set(HE_DEV_AGENT_RUNTIME_SOURCES
	AgentProtocol.h
	AgentCommandRegistry.h AgentCommandRegistry.cpp
	AgentEventLog.h AgentEventLog.cpp
	AgentSpdlogSink.h AgentSpdlogSink.cpp
	AgentInputController.h AgentInputController.cpp
	AgentCaptureController.h AgentCaptureController.cpp
	AgentTransport.h
	FileMailboxAgentTransport.h FileMailboxAgentTransport.cpp
	DevAgentRuntime.h DevAgentRuntime.cpp
)

add_library(HEngineDevAgentRuntime STATIC ${HE_DEV_AGENT_RUNTIME_SOURCES})

target_include_directories(HEngineDevAgentRuntime
	PUBLIC "${EngineSourceDir}"
	PRIVATE "${EngineRuntimeDir}"
)

target_compile_definitions(HEngineRuntime PUBLIC HE_DEV_AGENT_RUNTIME=1)
target_compile_definitions(HEngineDevAgentRuntime PUBLIC HE_DEV_AGENT_RUNTIME=1)
target_link_libraries(HEngineDevAgentRuntime PUBLIC HEngineRuntime)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${HE_DEV_AGENT_RUNTIME_SOURCES})
set_target_properties(HEngineDevAgentRuntime PROPERTIES FOLDER "Game/Agent")
