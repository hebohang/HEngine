file(GLOB EditorHeaderFiles CONFIGUE_DEPENDS "*.h" "Panels/*.h" "ImGuiWrapper/*.h")
file(GLOB EditorSourceFiles CONFIGUE_DEPENDS "*.cpp" "Panels/*.cpp" "ImGuiWrapper/*.cpp")

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


