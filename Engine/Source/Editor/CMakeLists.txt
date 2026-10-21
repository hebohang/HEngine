# learned from https://github.com/colmap/colmap/blob/9f3a75ae9c72188244f2403eb085e51ecf4397a8/cmake/CMakeHelper.cmake
macro(EDITOR_ADD_SOURCE)
	set(HEADER_FILES "")
	set(SOURCE_FILES "")
    foreach(SOURCE_FILE ${ARGN})
        if(SOURCE_FILE MATCHES "(\.h$)|(\.hpp$)")
            list(APPEND HEADER_FILES
                 "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}")
        elseif(SOURCE_FILE MATCHES "(\.cpp$)|(\.cc$)")
            list(APPEND SOURCE_FILES
                 "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}")
        endif()
    endforeach()
	set(EditorHeaderFiles ${EditorHeaderFiles} ${HEADER_FILES} PARENT_SCOPE)
    set(EditorSourceFiles ${EditorSourceFiles} ${SOURCE_FILES} PARENT_SCOPE)
endmacro(EDITOR_ADD_SOURCE)
# end from

set(EditorHeaderFiles
	"${CMAKE_CURRENT_SOURCE_DIR}/Automation/EditorSceneMailbox.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/EditorLayer.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/EditorHistory.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/EditorTheme.h")
set(EditorSourceFiles
    "${CMAKE_CURRENT_SOURCE_DIR}/Panels/BuildPublishPanel.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/Automation/EditorSceneMailbox.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/EditorLayer.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/EditorHistory.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/EditorTheme.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/HEngineEditorApp.cpp")

set(EditorResourceFiles "")
if(WIN32 AND EXISTS "${EngineResourcesDir}/Windows/Editor.rc")
	list(APPEND EditorResourceFiles "${EngineResourcesDir}/Windows/Editor.rc")
endif()

add_subdirectory(Panels)
add_subdirectory(ImGuiWrapper)
add_subdirectory(Manager)
add_subdirectory(Preview)

source_group(TREE ${EngineEditorDir} FILES ${EditorHeaderFiles} ${EditorSourceFiles})
if(EditorResourceFiles)
	source_group("Resources" FILES ${EditorResourceFiles})
endif()

add_executable(HEngineEditor ${EditorSourceFiles} ${EditorResourceFiles})
he_deploy_fsr31(HEngineEditor)
if(MSVC)
	target_compile_options(HEngineEditor PRIVATE /bigobj)
endif()
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/AssetToolsBuildConfig.h"
    "#pragma once\n#define HE_ASSET_TOOLS_PYTHON R\"(${Python3_EXECUTABLE})\"\n")
file(MAKE_DIRECTORY "${BINARY_ROOT_DIR}/Tools/Content")
foreach(assetTool asset_operations.py content_sync.py)
    configure_file("${CMAKE_SOURCE_DIR}/Tools/Content/${assetTool}"
        "${BINARY_ROOT_DIR}/Tools/Content/${assetTool}" COPYONLY)
endforeach()
find_program(HE_PLUGIN_SETTINGS_POWERSHELL NAMES pwsh powershell.exe pwsh.exe REQUIRED)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/PluginPanelBuildConfig.h"
    "#pragma once\n#define HE_PLUGIN_SETTINGS_POWERSHELL R\"(${HE_PLUGIN_SETTINGS_POWERSHELL})\"\n#define HE_PLUGIN_SETTINGS_SCRIPT R\"(${CMAKE_SOURCE_DIR}/Tools/Plugins/ApplyConfig.ps1)\"\n#define HE_PLUGIN_SETTINGS_CMAKE R\"(${CMAKE_COMMAND})\"\n")
target_include_directories(HEngineEditor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
add_dependencies(HEngineEditor HEngineRuntimeShaders)

if(WIN32 AND HE_RENDER_DX12)
	he_enable_d3d12_agility(HEngineEditor)
endif()

set_target_properties(HEngineEditor PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BINARY_ROOT_DIR}")
set_target_properties(HEngineEditor PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG "${BINARY_ROOT_DIR}")
set_target_properties(HEngineEditor PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${BINARY_ROOT_DIR}")

target_compile_definitions(HEngineRuntime PUBLIC
    WITH_EDITOR
)

target_link_libraries(HEngineEditor 
	PRIVATE HEngineRuntime
	PRIVATE imgui_node_editor
    PRIVATE ImGuizmo
)

he_attach_static_plugins(HEngineEditor Editor)

target_include_directories(HEngineEditor PRIVATE
	"${EngineSourceDir}"
	"${EngineRuntimeDir}"
)

target_compile_definitions(HEngineEditor PRIVATE
	$<$<PLATFORM_ID:Windows>:HE_PLATFORM_WINDOWS>
	_CRT_SECURE_NO_WARNINGS
)

set_target_properties(HEngineEditor PROPERTIES FOLDER "Engine")

# if(WIN32)
# 	set_target_properties(HEngineEditor PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup /SUBSYSTEM:WINDOWS")
# endif()

if(MSVC)
	set_target_properties(
	HEngineEditor PROPERTIES
	VS_DEBUGGER_WORKING_DIRECTORY "${EngineEditorDir}")
endif()

set(HEngineEditorHeaders "${EditorHeaderFiles}" PARENT_SCOPE)
