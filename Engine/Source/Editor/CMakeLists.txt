file(COPY ${EngineEditorDir}/Resources
DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

file(COPY ${EngineEditorDir}/imgui.ini
DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

file(GLOB EditorHeaderFiles CONFIGUE_DEPENDS "*.h" "Panels/*.h")
file(GLOB EditorSourceFiles CONFIGUE_DEPENDS "*.cpp" "Panels/*.cpp")

source_group(TREE ${EngineEditorDir} FILES ${EditorHeaderFiles} ${EditorSourceFiles})

add_executable(HEngineEditor ${EditorHeaderFiles} ${EditorSourceFiles})

target_link_libraries(HEngineEditor PRIVATE HEngineRuntime)

target_include_directories(HEngineEditor PRIVATE
	"${EngineSourceDir}"
	"${EngineRuntimeDir}"
)

target_compile_definitions(HEngineEditor PRIVATE
	HE_PLATFORM_WINDOWS
	HE_DEBUG
	_CRT_SECURE_NO_WARNINGS
)

if(MSVC)
	set_target_properties(
	HEngineEditor PROPERTIES
	VS_DEBUGGER_WORKING_DIRECTORY "${EngineEditorDir}")
endif()


