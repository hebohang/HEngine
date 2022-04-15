file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/Resources
DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/imgui.ini
DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

file(GLOB HEngineEditorSrc
"*.h"
"*.cpp"
"Panels/*.h"
"Panels/*.cpp"
)

source_group(TREE ${CMAKE_SOURCE_DIR}/Engine/Source/Editor FILES ${HEngineEditorSrc})

add_executable(HEngineEditor ${HEngineEditorSrc})

target_link_libraries(HEngineEditor PRIVATE HEngineRuntime)

target_include_directories(HEngineEditor
PRIVATE
"../Runtime/ThirdParty/spdlog/include"
"../Runtime"
"../Runtime/HEngine"
"../Runtime/ThirdParty"
"../Runtime/ThirdParty/glm"
"../Runtime/ThirdParty/entt/include"
"../Runtime/ThirdParty/imguizmo"
"../Runtime/ThirdParty/Box2D"
)

target_compile_definitions(HEngineEditor PRIVATE
HE_PLATFORM_WINDOWS
HE_DEBUG
_CRT_SECURE_NO_WARNINGS
)

if(MSVC)
	set_target_properties(
	HEngineEditor PROPERTIES
	VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/Engine/Source/HEngineEditor")
endif()


