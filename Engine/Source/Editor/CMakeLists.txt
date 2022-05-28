# learned from https://github.com/colmap/colmap/blob/9f3a75ae9c72188244f2403eb085e51ecf4397a8/cmake/CMakeHelper.cmake
macro(EDITOR_ADD_SOURCE)
	set(SOURCE_FILES "")
    foreach(SOURCE_FILE ${ARGN})
        if(SOURCE_FILE MATCHES "^/.*")
            list(APPEND SOURCE_FILES ${SOURCE_FILE})
        else()
            list(APPEND SOURCE_FILES
                 "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}")
        endif()
    endforeach()
    set(EditorSourceFiles ${EditorSourceFiles} ${SOURCE_FILES} PARENT_SCOPE)
endmacro(EDITOR_ADD_SOURCE)
# end from

set(EditorSourceFiles ${CMAKE_CURRENT_SOURCE_DIR}/EditorLayer.h ${CMAKE_CURRENT_SOURCE_DIR}/EditorLayer.cpp ${CMAKE_CURRENT_SOURCE_DIR}/HEngineEditorApp.cpp)

add_subdirectory(Panels)
add_subdirectory(ImGuiWrapper)
add_subdirectory(IconManager)

source_group(TREE ${EngineEditorDir} FILES ${EditorSourceFiles})

add_executable(HEngineEditor ${EditorSourceFiles})

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
