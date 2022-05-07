file(GLOB EditorHeaderFiles CONFIGUE_DEPENDS "*.h" "Panels/*.h" "ImGuiWrapper/*.h" "IconManager/*.h")
file(GLOB EditorSourceFiles CONFIGUE_DEPENDS "*.cpp" "Panels/*.cpp" "ImGuiWrapper/*.cpp" "IconManager/*.cpp")

source_group(TREE ${EngineEditorDir} FILES ${EditorHeaderFiles} ${EditorSourceFiles})

add_executable(HEngineEditor ${EditorHeaderFiles} ${EditorSourceFiles})

add_compile_definitions("ENGINE_ROOT_DIR=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

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

add_custom_command(TARGET HEngineEditor POST_BUILD
				   COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/"
                   COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${EngineConfigDir}/imgui.ini" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
                   COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${EngineConfigDir}/imgui.ini" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/"
)
