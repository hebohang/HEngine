macro(GAME_ADD_SOURCE)
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
	set(GameHeaderFiles ${GameHeaderFiles} ${HEADER_FILES} PARENT_SCOPE)
    set(GameSourceFiles ${GameSourceFiles} ${SOURCE_FILES} PARENT_SCOPE)
endmacro(GAME_ADD_SOURCE)

set(GameHeaderFiles "")
set(ProjectGameSourceDir "${ProjectRootDir}/Source/Game")
if(NOT EXISTS "${ProjectGameSourceDir}/Game.cpp" OR
   NOT EXISTS "${ProjectGameSourceDir}/GameLayer/CMakeLists.txt")
	message(FATAL_ERROR "Project game sources are missing: ${ProjectGameSourceDir}")
endif()
set(GameSourceFiles "${ProjectGameSourceDir}/Game.cpp")
set(GameResourceFiles "")

if(WIN32 AND EXISTS "${ProjectRootDir}/Resources/Windows/Game.rc")
	list(APPEND GameResourceFiles "${ProjectRootDir}/Resources/Windows/Game.rc")
endif()

add_subdirectory("${ProjectGameSourceDir}/GameLayer" "${CMAKE_CURRENT_BINARY_DIR}/ProjectGameLayer")

if(HE_BUILD_DEV_GAME AND NOT PACK_GAME AND HE_ENABLE_GM)
	add_subdirectory(AgentRuntime)
endif()

source_group(TREE "${ProjectGameSourceDir}" FILES ${GameHeaderFiles} ${GameSourceFiles})
source_group("Resources" FILES ${GameResourceFiles})

add_executable(HGame ${GameSourceFiles} ${GameResourceFiles})
add_dependencies(HGame HEngineRuntimeShaders)

if(WIN32)
	he_enable_d3d12_agility(HGame)
endif()

if(PACK_GAME)
	set_target_properties(HGame PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
	)
else()
	set_target_properties(HGame PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BINARY_ROOT_DIR}")
	set_target_properties(HGame PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG "${BINARY_ROOT_DIR}")
	set_target_properties(HGame PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${BINARY_ROOT_DIR}")
endif()

if(PACK_GAME)
	target_compile_definitions(HEngineRuntime PUBLIC
		HE_PACKAGED_GAME=1
	)
endif()

target_link_libraries(HGame 
	PRIVATE HEngineRuntime
)

if(PACK_GAME)
	he_attach_static_plugins(HGame Game)
else()
	he_attach_static_plugins(HGame DevGame)
endif()

if(HE_BUILD_DEV_GAME AND NOT PACK_GAME AND HE_ENABLE_GM)
	target_link_libraries(HGame PRIVATE HEngineDevAgentRuntime)
endif()

target_include_directories(HGame PRIVATE
	"${ProjectGameSourceDir}"
	"${EngineSourceDir}"
	"${EngineRuntimeDir}"
)

target_compile_definitions(HGame PRIVATE
	HE_PLATFORM_WINDOWS
	_CRT_SECURE_NO_WARNINGS
)

set_target_properties(HGame PROPERTIES
	FOLDER "Game"
	OUTPUT_NAME "${HE_PROJECT_EXECUTABLE_NAME}"
	INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
)

set_property(TARGET HEngineRuntime PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

if(MSVC)
	# The project RC file embeds the application manifest.
	target_link_options(HGame PRIVATE /MANIFEST:NO)
	if(PACK_GAME)
		set(HE_PACKAGE_POLICY_OUTPUT_DIR
			"${CMAKE_BINARY_DIR}/PackagePolicy")
		file(MAKE_DIRECTORY "${HE_PACKAGE_POLICY_OUTPUT_DIR}")
		set(HE_PACKAGE_LINK_MAP
			"${HE_PACKAGE_POLICY_OUTPUT_DIR}/HGame-Release.map")
		set(HE_PACKAGE_TARGET_GRAPH
			"${CMAKE_BINARY_DIR}/PackageTargetGraph.dot")
		file(WRITE "${CMAKE_BINARY_DIR}/PackageLinkMap.path"
			"${HE_PACKAGE_LINK_MAP}\n")
		target_link_options(HGame PRIVATE
			$<$<CONFIG:Release>:/OPT:REF>
			$<$<CONFIG:Release>:/OPT:ICF>
			$<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>
			$<$<CONFIG:Release>:/ENTRY:mainCRTStartup>
			$<$<CONFIG:Release>:/MAP:${HE_PACKAGE_LINK_MAP}>
			$<$<CONFIG:Release>:/MAPINFO:EXPORTS>
		)
	endif()
endif()

if(PACK_GAME)
file(REAL_PATH "${ProjectRootDir}" HE_PROJECT_CANONICAL_ROOT)
function(he_resolve_owned_project_file relativePath outputVariable description)
	set(candidatePath "${ProjectRootDir}/${relativePath}")
	if(NOT EXISTS "${candidatePath}" OR IS_DIRECTORY "${candidatePath}" OR
	   IS_SYMLINK "${candidatePath}")
		message(FATAL_ERROR "${description} must be a regular project-owned file: ${relativePath}")
	endif()
	get_filename_component(candidateAbsolute "${candidatePath}" ABSOLUTE)
	file(REAL_PATH "${candidateAbsolute}" candidateReal)
	file(RELATIVE_PATH candidateLexicalRelative "${ProjectRootDir}" "${candidateAbsolute}")
	file(RELATIVE_PATH candidateRealRelative "${HE_PROJECT_CANONICAL_ROOT}" "${candidateReal}")
	string(REPLACE "\\" "/" candidateLexicalRelative "${candidateLexicalRelative}")
	string(REPLACE "\\" "/" candidateRealRelative "${candidateRealRelative}")
	if(IS_ABSOLUTE "${candidateRealRelative}" OR
	   candidateRealRelative MATCHES "(^|/)\\.\\.(/|$)")
		message(FATAL_ERROR "${description} resolves outside the selected project: ${relativePath}")
	endif()
	if(WIN32)
		string(TOLOWER "${candidateLexicalRelative}" candidateLexicalKey)
		string(TOLOWER "${candidateRealRelative}" candidateRealKey)
	else()
		set(candidateLexicalKey "${candidateLexicalRelative}")
		set(candidateRealKey "${candidateRealRelative}")
	endif()
	if(NOT candidateLexicalKey STREQUAL candidateRealKey)
		message(FATAL_ERROR "${description} crosses a project symlink or junction: ${relativePath}")
	endif()
	set(${outputVariable} "${candidateReal}" PARENT_SCOPE)
endfunction()

file(READ "${ProjectPackageConfigFile}" ProjectPackageConfigJson)
string(JSON ProjectThirdPartyNotices ERROR_VARIABLE ProjectNoticesError
	GET "${ProjectPackageConfigJson}" ThirdPartyNotices)
string(JSON ProjectLicenseCount ERROR_VARIABLE ProjectLicensesError
	LENGTH "${ProjectPackageConfigJson}" Licenses)
string(REPLACE "\\" "/" ProjectThirdPartyNotices "${ProjectThirdPartyNotices}")
if(ProjectNoticesError OR IS_ABSOLUTE "${ProjectThirdPartyNotices}" OR
	ProjectThirdPartyNotices MATCHES "(^|/)\\.\\.(/|$)")
	message(FATAL_ERROR "PackageConfig ThirdPartyNotices must be a project-relative path")
endif()
if(ProjectLicensesError)
	message(FATAL_ERROR "PackageConfig Licenses must be an array")
endif()
he_resolve_owned_project_file(
	"${ProjectThirdPartyNotices}"
	ProjectThirdPartyNoticesFile
	"PackageConfig ThirdPartyNotices")

# Publishing is fixed beneath the repository-owned, gitignored dist boundary.
# External projects receive a caller-provided hash namespace so two directories
# with the same project identity cannot replace each other's package.
set(HE_PACKAGE_OUTPUT_KEY "" CACHE STRING
	"Sixteen lowercase hex characters used to namespace an external project package.")
file(REAL_PATH "${RootDir}/Project" HE_DEFAULT_PROJECT_CANONICAL_ROOT)
if(WIN32)
	string(TOLOWER "${HE_PROJECT_CANONICAL_ROOT}" HE_PROJECT_CANONICAL_KEY)
	string(TOLOWER "${HE_DEFAULT_PROJECT_CANONICAL_ROOT}" HE_DEFAULT_PROJECT_CANONICAL_KEY)
else()
	set(HE_PROJECT_CANONICAL_KEY "${HE_PROJECT_CANONICAL_ROOT}")
	set(HE_DEFAULT_PROJECT_CANONICAL_KEY "${HE_DEFAULT_PROJECT_CANONICAL_ROOT}")
endif()
if(HE_PROJECT_CANONICAL_KEY STREQUAL HE_DEFAULT_PROJECT_CANONICAL_KEY)
	if(NOT HE_PACKAGE_OUTPUT_KEY STREQUAL "")
		message(FATAL_ERROR "The default project package must not use an external output key")
	endif()
	set(HE_GAME_PACKAGE_OUTPUT_ROOT "${RootDir}/dist")
else()
	string(LENGTH "${HE_PACKAGE_OUTPUT_KEY}" HE_PACKAGE_OUTPUT_KEY_LENGTH)
	if(NOT HE_PACKAGE_OUTPUT_KEY_LENGTH EQUAL 16 OR
	   NOT HE_PACKAGE_OUTPUT_KEY MATCHES "^[0-9a-f]+$")
		message(FATAL_ERROR
			"External PackageGame builds require HE_PACKAGE_OUTPUT_KEY as sixteen lowercase hex characters; use hectl package -Project")
	endif()
	set(HE_GAME_PACKAGE_OUTPUT_ROOT "${RootDir}/dist/projects/${HE_PACKAGE_OUTPUT_KEY}")
endif()
get_filename_component(HE_GAME_PACKAGE_OUTPUT_ROOT
	"${HE_GAME_PACKAGE_OUTPUT_ROOT}" ABSOLUTE)
set(HE_GAME_PACKAGE_DIR
	"${HE_GAME_PACKAGE_OUTPUT_ROOT}/${HE_PROJECT_EXECUTABLE_NAME}")
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef HE_GAME_PACKAGE_NONCE)
set(HE_GAME_PACKAGE_STAGING_DIR "${HE_GAME_PACKAGE_DIR}.staging-${HE_GAME_PACKAGE_NONCE}")
set(HE_GAME_COOK_INPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/CookInput-${HE_GAME_PACKAGE_NONCE}")
set(HE_GAME_COOK_REPORT "${CMAKE_CURRENT_BINARY_DIR}/CookReport.json")
set(HE_GAME_PACKAGE_MARKER "${CMAKE_CURRENT_BINARY_DIR}/PackageRoot.marker")
set(HE_GAME_PACKAGE_README "${CMAKE_CURRENT_BINARY_DIR}/PackageREADME.txt")
file(WRITE "${HE_GAME_PACKAGE_MARKER}" "HEngine ${HE_PROJECT_ID} Package v1\n")
configure_file(
	"${RootDir}/CMake/WindowsGamePackageReadme.txt.in"
	"${HE_GAME_PACKAGE_README}"
	@ONLY)
file(WRITE "${CMAKE_BINARY_DIR}/GamePackage.directory" "${HE_GAME_PACKAGE_DIR}\n")

if(NOT MSVC)
	message(FATAL_ERROR "PackageGame requires the supported MSVC v143 toolchain")
endif()
set(HE_VC_REDIST_X64_DIR "" CACHE PATH
	"Optional fixed Visual Studio v143 x64 CRT redistributable directory.")
if(NOT HE_VC_REDIST_X64_DIR)
	if(DEFINED ENV{VCToolsRedistDir} AND NOT "$ENV{VCToolsRedistDir}" STREQUAL "")
		file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}" HE_VC_REDIST_ROOT)
		set(HE_VC_REDIST_X64_DIR
			"${HE_VC_REDIST_ROOT}/x64/Microsoft.VC143.CRT")
	elseif(CMAKE_GENERATOR_INSTANCE)
		get_filename_component(HE_MSVC_COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
		get_filename_component(HE_MSVC_COMPILER_HOST "${HE_MSVC_COMPILER_BIN}" DIRECTORY)
		get_filename_component(HE_MSVC_COMPILER_BIN_ROOT "${HE_MSVC_COMPILER_HOST}" DIRECTORY)
		get_filename_component(HE_MSVC_TOOLSET_ROOT "${HE_MSVC_COMPILER_BIN_ROOT}" DIRECTORY)
		get_filename_component(HE_MSVC_TOOLSET_VERSION "${HE_MSVC_TOOLSET_ROOT}" NAME)
		string(REGEX MATCH "^[0-9]+[.][0-9]+" HE_MSVC_TOOLSET_FAMILY
			"${HE_MSVC_TOOLSET_VERSION}")
		file(GLOB HE_VC_REDIST_VERSION_DIRS LIST_DIRECTORIES TRUE
			"${CMAKE_GENERATOR_INSTANCE}/VC/Redist/MSVC/${HE_MSVC_TOOLSET_FAMILY}.*")
		list(SORT HE_VC_REDIST_VERSION_DIRS COMPARE NATURAL ORDER DESCENDING)
		foreach(HE_VC_REDIST_VERSION_DIR IN LISTS HE_VC_REDIST_VERSION_DIRS)
			set(HE_VC_REDIST_CANDIDATE
				"${HE_VC_REDIST_VERSION_DIR}/x64/Microsoft.VC143.CRT")
			if(EXISTS "${HE_VC_REDIST_CANDIDATE}/vcruntime140.dll")
				set(HE_VC_REDIST_X64_DIR "${HE_VC_REDIST_CANDIDATE}")
				break()
			endif()
		endforeach()
	endif()
endif()
if(NOT HE_VC_REDIST_X64_DIR)
	message(FATAL_ERROR
		"Could not locate the Visual Studio v143 redistributable matching "
		"${CMAKE_CXX_COMPILER}. Set HE_VC_REDIST_X64_DIR to the licensed "
		"x64/Microsoft.VC143.CRT directory.")
endif()
get_filename_component(HE_VC_REDIST_X64_DIR "${HE_VC_REDIST_X64_DIR}" ABSOLUTE)
set(HE_VC_REDIST_X64_DIR "${HE_VC_REDIST_X64_DIR}" CACHE PATH
	"Fixed Visual Studio v143 x64 CRT redistributable directory." FORCE)
set(HE_PACKAGE_VCRUNTIME "${HE_VC_REDIST_X64_DIR}/vcruntime140.dll")
if(NOT EXISTS "${HE_PACKAGE_VCRUNTIME}" OR IS_DIRECTORY "${HE_PACKAGE_VCRUNTIME}")
	message(FATAL_ERROR
		"The official Visual Studio v143 x64 redistributable is missing vcruntime140.dll: "
		"${HE_PACKAGE_VCRUNTIME}")
endif()
file(TO_CMAKE_PATH "$ENV{SystemRoot}/System32" HE_WINDOWS_SYSTEM32_DIR)
string(TOLOWER "${HE_PACKAGE_VCRUNTIME}" HE_PACKAGE_VCRUNTIME_KEY)
string(TOLOWER "${HE_WINDOWS_SYSTEM32_DIR}/" HE_WINDOWS_SYSTEM32_KEY)
string(FIND "${HE_PACKAGE_VCRUNTIME_KEY}" "${HE_WINDOWS_SYSTEM32_KEY}" HE_SYSTEM32_RUNTIME_POSITION)
if(HE_SYSTEM32_RUNTIME_POSITION EQUAL 0)
	message(FATAL_ERROR "PackageGame must never source VC runtime files from System32")
endif()
message(STATUS "Package VC runtime source: ${HE_PACKAGE_VCRUNTIME}")
find_program(HE_WINDOWS_POWERSHELL_EXECUTABLE NAMES powershell.exe
	HINTS "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0"
	REQUIRED)

if(NOT TARGET HCooker)
	message(FATAL_ERROR "HCooker target is required for PACK_GAME builds")
endif()
get_property(HE_PACKAGE_RUNTIME_SHADER_SOURCES GLOBAL PROPERTY HE_RUNTIME_SHADER_SOURCES)
if(NOT HE_PACKAGE_RUNTIME_SHADER_SOURCES)
	message(FATAL_ERROR "The runtime shader source allow-list is unavailable")
endif()
set(HE_PREPARE_COOK_INPUT_COMMAND
	COMMAND ${CMAKE_COMMAND}
		-DENGINE_CONTENT_DIR=${EngineContentDir}
		-DENGINE_GENERATED_CONTENT_DIR=${EngineGeneratedContentDir}
		-DPROJECT_DIR=${ProjectRootDir}
		-DCOOK_INPUT_DIR=${HE_GAME_COOK_INPUT_DIR}
		-DCOOK_INPUT_PARENT=${CMAKE_CURRENT_BINARY_DIR}
		-DPROJECT_ID=${HE_PROJECT_ID}
		-P "${RootDir}/CMake/PrepareCookInput.cmake")
set(HE_PACKAGE_SHADER_COMMANDS
	COMMAND ${CMAKE_COMMAND}
		-DGENERATED_CONTENT_DIR=${EngineGeneratedContentDir}
		-DGENERATED_CONTENT_PARENT=${CMAKE_BINARY_DIR}
		-P "${RootDir}/CMake/ResetPackageShaderCache.cmake")
foreach(shaderSource IN LISTS HE_PACKAGE_RUNTIME_SHADER_SOURCES)
	list(APPEND HE_PACKAGE_SHADER_COMMANDS
		COMMAND $<TARGET_FILE:HShaderCompiler>
			--rootPath "${EngineContentShadersDir}"
			--outputPath "${EngineGeneratedShadersDir}"
			--shader "${shaderSource}"
			--binary-format all)
endforeach()
if(IS_DIRECTORY "${ProjectShadersDir}")
	file(GLOB_RECURSE HE_PACKAGE_PROJECT_SHADER_SOURCES
		CONFIGURE_DEPENDS
		"${ProjectShadersDir}/*.hlsl"
		"${ProjectShadersDir}/*.hlsli")
	list(FILTER HE_PACKAGE_PROJECT_SHADER_SOURCES EXCLUDE REGEX
		"[/\\]DevOnly([/\\]|$)")
	foreach(projectShaderSource IN LISTS HE_PACKAGE_PROJECT_SHADER_SOURCES)
		if(projectShaderSource MATCHES "[.]hlsl$")
			list(APPEND HE_PACKAGE_SHADER_COMMANDS
				COMMAND $<TARGET_FILE:HShaderCompiler>
					--shader "${projectShaderSource}"
					--rootPath "${ProjectShadersDir}"
					--outputPath "${EngineGeneratedShadersDir}"
					--includePath "${EngineContentShadersDir}"
					--binary-format all)
		endif()
	endforeach()
endif()
list(APPEND HE_PACKAGE_SHADER_COMMANDS
	COMMAND ${CMAKE_COMMAND} -E remove_directory
		"${EngineGeneratedShadersDir}/CACHES/.artifact-owners"
	COMMAND ${CMAKE_COMMAND} -E touch
		"${EngineGeneratedContentDir}/.runtime-shaders.stamp")

add_custom_target(PackageGame
	COMMAND ${CMAKE_COMMAND}
		-DBUILD_CONFIG=$<CONFIG>
		-DPACK_GAME=${PACK_GAME}
		-DHE_BUILD_DEV_GAME=${HE_BUILD_DEV_GAME}
		-DHE_ENABLE_GM=${HE_ENABLE_GM}
		-P "${RootDir}/CMake/VerifyPackageConfiguration.cmake"
	${HE_PACKAGE_SHADER_COMMANDS}
	${HE_PREPARE_COOK_INPUT_COMMAND}
	COMMAND ${CMAKE_COMMAND}
		-DPROJECT_DIR=${ProjectRootDir}
		-DPACKAGE_OUTPUT_ROOT=${HE_GAME_PACKAGE_OUTPUT_ROOT}
		-DPACKAGE_DIR=${HE_GAME_PACKAGE_DIR}
		-DSTAGING_DIR=${HE_GAME_PACKAGE_STAGING_DIR}
		-DPACKAGE_EXECUTABLE_NAME=${HE_PROJECT_EXECUTABLE_NAME}
		-DPACKAGE_PROJECT_ID=${HE_PROJECT_ID}
		-DPACKAGE_NONCE=${HE_GAME_PACKAGE_NONCE}
		-P "${RootDir}/CMake/PrepareGamePackageStaging.cmake"
	COMMAND $<TARGET_FILE:HCooker> --self-test
	COMMAND $<TARGET_FILE:HCooker>
		--project "${HE_GAME_COOK_INPUT_DIR}"
		--engine-config "${EngineContentDir}/Config/EngineCook.json"
		--forbid-root "${ProjectRootDir}"
		--forbid-root "${EngineContentDir}"
		--forbid-root "${EngineGeneratedContentDir}"
		--config "${HE_PROJECT_GAME_CONFIG}"
		--output "${HE_GAME_PACKAGE_STAGING_DIR}"
		--report "${HE_GAME_COOK_REPORT}"
		--run-id "${HE_GAME_PACKAGE_NONCE}"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		$<TARGET_FILE:HGame>
		"${HE_GAME_PACKAGE_STAGING_DIR}/${HE_PROJECT_EXECUTABLE_NAME}.exe"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${ThirdPartyDir}/Python/dlls/python311.dll"
		"${HE_GAME_PACKAGE_STAGING_DIR}/python311.dll"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${HE_PACKAGE_VCRUNTIME}"
		"${HE_GAME_PACKAGE_STAGING_DIR}/vcruntime140.dll"
	COMMAND ${CMAKE_COMMAND} -E make_directory
		"${HE_GAME_PACKAGE_STAGING_DIR}/D3D12"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${HE_D3D12_AGILITY_CORE_DLL}"
		"${HE_GAME_PACKAGE_STAGING_DIR}/D3D12/D3D12Core.dll"
	COMMAND ${CMAKE_COMMAND} -E make_directory
		"${HE_GAME_PACKAGE_STAGING_DIR}/Licenses"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${RootDir}/LICENSE"
		"${HE_GAME_PACKAGE_STAGING_DIR}/LICENSE.txt"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${HE_GAME_PACKAGE_README}"
		"${HE_GAME_PACKAGE_STAGING_DIR}/README.txt"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${ProjectThirdPartyNoticesFile}"
		"${HE_GAME_PACKAGE_STAGING_DIR}/THIRD_PARTY_NOTICES.txt"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${HE_GAME_PACKAGE_MARKER}"
		"${HE_GAME_PACKAGE_STAGING_DIR}/.he-package-root"
	WORKING_DIRECTORY "$<TARGET_FILE_DIR:HShaderCompiler>"
	COMMENT "Cooking and assembling ${HE_PROJECT_NAME}"
	VERBATIM
)

add_dependencies(PackageGame HGame HCooker)
set_target_properties(PackageGame PROPERTIES FOLDER "Game")

function(he_package_license sourcePath outputName)
	if(NOT EXISTS "${sourcePath}")
		message(FATAL_ERROR "Required third-party license is missing: ${sourcePath}")
	endif()
	add_custom_command(TARGET PackageGame POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${sourcePath}"
			"${HE_GAME_PACKAGE_STAGING_DIR}/Licenses/${outputName}"
		VERBATIM
	)
	list(APPEND HE_GAME_PACKAGE_LICENSE_FILES "${outputName}")
	set(HE_GAME_PACKAGE_LICENSE_FILES "${HE_GAME_PACKAGE_LICENSE_FILES}" PARENT_SCOPE)
endfunction()

set(HE_GAME_PACKAGE_LICENSE_FILES "")
he_package_license("${ThirdPartyDir}/GLFW/LICENSE.md" "GLFW.txt")
he_package_license("${ThirdPartyDir}/glm/copying.txt" "GLM.txt")
he_package_license("${ThirdPartyDir}/imgui/LICENSE.txt" "DearImGui.txt")
he_package_license("${ThirdPartyDir}/spdlog/LICENSE" "spdlog.txt")
he_package_license("${ThirdPartyDir}/spdlog/include/spdlog/fmt/bundled/fmt.license.rst" "fmt.txt")
he_package_license("${ThirdPartyDir}/Box2D/LICENSE" "Box2D.txt")
he_package_license("${EngineResourcesDir}/Licenses/stb.txt" "stb.txt")
he_package_license("${ThirdPartyDir}/JoltPhysics/LICENSE" "JoltPhysics.txt")
he_package_license("${ThirdPartyDir}/pybind11/LICENSE" "pybind11.txt")
he_package_license("${ThirdPartyDir}/json/LICENSE.MIT" "nlohmann-json.txt")
he_package_license("${ThirdPartyDir}/magic_enum/LICENSE" "magic-enum.txt")
he_package_license("${ThirdPartyDir}/entt/LICENSE" "EnTT.txt")
he_package_license("${ThirdPartyDir}/crossguid/LICENSE" "crossguid.txt")
he_package_license("${ThirdPartyDir}/simpleIni/licence.txt" "SimpleIni.txt")
he_package_license("${ThirdPartyDir}/miniaudio/LICENSE" "miniaudio.txt")
he_package_license("${ThirdPartyDir}/Effekseer/LICENSE.txt" "Effekseer.txt")
he_package_license("${ThirdPartyDir}/Effekseer/LICENSE_RUNTIME_DIRECTX.txt" "Effekseer-DirectX.txt")
he_package_license("${ThirdPartyDir}/Effekseer/LICENSE_RUNTIME_VULKAN.txt" "Effekseer-Vulkan.txt")
he_package_license("${ThirdPartyDir}/recastnavigation/License.txt" "RecastNavigation.txt")
he_package_license("${ThirdPartyDir}/RmlUi/LICENSE.txt" "RmlUi.txt")
he_package_license("${ThirdPartyDir}/LunaSVG/LICENSE" "LunaSVG.txt")
he_package_license("${ThirdPartyDir}/PlutoVG/LICENSE" "PlutoVG.txt")
he_package_license("${ThirdPartyDir}/rlottie/COPYING" "rlottie.txt")
he_package_license("${ThirdPartyDir}/rlottie/licenses/COPYING.FTL" "rlottie-FreeType.txt")
he_package_license("${ThirdPartyDir}/rlottie/licenses/COPYING.MIT" "rlottie-MIT.txt")
he_package_license("${ThirdPartyDir}/rlottie/licenses/COPYING.MPL" "rlottie-MPL.txt")
he_package_license("${ThirdPartyDir}/rlottie/licenses/COPYING.PIX" "rlottie-Pixman.txt")
he_package_license("${ThirdPartyDir}/rlottie/licenses/COPYING.RPD" "rlottie-RapidJSON.txt")
he_package_license("${ThirdPartyDir}/rlottie/licenses/COPYING.SKIA" "rlottie-Skia.txt")
he_package_license("${ThirdPartyDir}/rlottie/licenses/COPYING.STB" "rlottie-stb.txt")
he_package_license("${ThirdPartyDir}/NVRHI/LICENSE.txt" "NVRHI.txt")
he_package_license("${ThirdPartyDir}/NVRHI/ThirdPartyLicenses.txt" "NVRHI-ThirdParty.txt")
he_package_license("${ThirdPartyDir}/Vulkan-Headers/LICENSE.md" "Vulkan-Headers.txt")
he_package_license("${HE_D3D12_AGILITY_RUNTIME_LICENSE}" "DirectX-Agility-SDK.txt")
he_package_license("${HE_D3D12_AGILITY_HEADERS_LICENSE}" "DirectX-Agility-SDK-Code.txt")
he_package_license("${EngineResourcesDir}/Licenses/Microsoft-Visual-Cpp-Runtime.txt" "Microsoft-Visual-Cpp-Runtime.txt")
he_package_license("${ThirdPartyDir}/freetype2/docs/FTL.TXT" "FreeType.txt")
he_package_license("${ThirdPartyDir}/assimp/contrib/zlib/LICENSE" "zlib.txt")
he_package_license("${ThirdPartyDir}/assimp/contrib/unzip/MiniZip64_info.txt" "MiniZip.txt")
he_package_license("${ThirdPartyDir}/assimp/contrib/zip/UNLICENSE" "kuba-zip.txt")
he_package_license("${EngineResourcesDir}/Licenses/Python-3.11.txt" "Python-3.11.txt")
he_package_license("${EngineContentAssetsDir}/Fonts/opensans/LICENSE.txt" "OpenSans.txt")
he_package_license("${EngineContentAssetsDir}/Fonts/Noto_Sans_SC/OFL.txt" "NotoSansSC.txt")
he_package_license(
	"${EngineContentDir}/SourceAssets/MaterialPreview/MaterialX-LICENSE.txt"
	"MaterialX.txt")
he_package_license(
	"${EngineContentAssetsDir}/Models/MaterialPreview/PROVENANCE.md"
	"MaterialX-ShaderBall-Attribution.txt")
he_package_license(
	"${EngineContentDir}/SourceAssets/MaterialPreview/MaterialX-THIRD-PARTY.md"
	"MaterialX-Third-Party.txt")

if(ProjectLicenseCount GREATER 0)
	math(EXPR ProjectLicenseLast "${ProjectLicenseCount} - 1")
	foreach(ProjectLicenseIndex RANGE ${ProjectLicenseLast})
		string(JSON ProjectLicenseSource ERROR_VARIABLE ProjectLicenseSourceError
			GET "${ProjectPackageConfigJson}" Licenses ${ProjectLicenseIndex} Source)
		string(JSON ProjectLicenseOutput ERROR_VARIABLE ProjectLicenseOutputError
			GET "${ProjectPackageConfigJson}" Licenses ${ProjectLicenseIndex} Output)
		string(REPLACE "\\" "/" ProjectLicenseSource "${ProjectLicenseSource}")
		if(ProjectLicenseSourceError OR IS_ABSOLUTE "${ProjectLicenseSource}" OR
			ProjectLicenseSource MATCHES "(^|/)\\.\\.(/|$)")
			message(FATAL_ERROR "PackageConfig license Source must be project-relative")
		endif()
		if(ProjectLicenseOutputError OR
			NOT ProjectLicenseOutput MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*[.]txt$")
			message(FATAL_ERROR "PackageConfig license Output must be a safe .txt filename")
		endif()
		string(TOUPPER "${ProjectLicenseOutput}" ProjectLicenseOutputUpper)
		string(LENGTH "${ProjectLicenseOutput}" ProjectLicenseOutputLength)
		if(ProjectLicenseOutputLength GREATER 128 OR
		   ProjectLicenseOutputUpper MATCHES "^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])([.].*)?$")
			message(FATAL_ERROR "PackageConfig license Output uses an unsafe Windows filename")
		endif()
		he_resolve_owned_project_file(
			"${ProjectLicenseSource}"
			ResolvedProjectLicenseSource
			"PackageConfig license Source")
		he_package_license("${ResolvedProjectLicenseSource}" "${ProjectLicenseOutput}")
	endforeach()
endif()

set(HE_GAME_PACKAGE_LICENSE_FILE_KEYS "")
foreach(licenseOutputName IN LISTS HE_GAME_PACKAGE_LICENSE_FILES)
	string(TOLOWER "${licenseOutputName}" licenseOutputKey)
	list(FIND HE_GAME_PACKAGE_LICENSE_FILE_KEYS "${licenseOutputKey}" licenseOutputIndex)
	if(NOT licenseOutputIndex EQUAL -1)
		message(FATAL_ERROR
			"Package license output names must be unique on Windows: ${licenseOutputName}")
	endif()
	list(APPEND HE_GAME_PACKAGE_LICENSE_FILE_KEYS "${licenseOutputKey}")
endforeach()
list(SORT HE_GAME_PACKAGE_LICENSE_FILES)
string(REPLACE ";" "\n" HE_GAME_PACKAGE_LICENSE_FILE_CONTENT "${HE_GAME_PACKAGE_LICENSE_FILES}")
set(HE_GAME_PACKAGE_LICENSE_LIST_FILE "${CMAKE_CURRENT_BINARY_DIR}/ExpectedPackageLicenses.txt")
file(WRITE "${HE_GAME_PACKAGE_LICENSE_LIST_FILE}" "${HE_GAME_PACKAGE_LICENSE_FILE_CONTENT}\n")

add_custom_command(TARGET PackageGame POST_BUILD
	COMMAND ${CMAKE_COMMAND}
		-DPACKAGE_DIR=${HE_GAME_PACKAGE_STAGING_DIR}
		-DPACKAGE_EXECUTABLE_NAME=${HE_PROJECT_EXECUTABLE_NAME}
		-DPACKAGE_PROJECT_ID=${HE_PROJECT_ID}
		-DPACKAGE_LICENSE_LIST_FILE=${HE_GAME_PACKAGE_LICENSE_LIST_FILE}
		-P "${RootDir}/CMake/VerifyGamePackage.cmake"
	COMMAND "${HE_WINDOWS_POWERSHELL_EXECUTABLE}"
		-NoProfile
		-NonInteractive
		-ExecutionPolicy Bypass
		-File "${RootDir}/Tools/Packaging/Verify-WindowsPackage.ps1"
		-PackageDir "${HE_GAME_PACKAGE_STAGING_DIR}"
		-ExecutableName "${HE_PROJECT_EXECUTABLE_NAME}"
		-ExpectedD3D12SDKVersion "${HE_D3D12_AGILITY_SDK_VERSION}"
		-ExpectedAgilityPackageVersion "${HE_D3D12_AGILITY_PACKAGE_VERSION}"
		-TargetGraphPath "${HE_PACKAGE_TARGET_GRAPH}"
		-LinkMapPath "${HE_PACKAGE_LINK_MAP}"
		-RequireTargetGraphAndLinkMap
	COMMAND "${HE_WINDOWS_POWERSHELL_EXECUTABLE}"
		-NoProfile
		-NonInteractive
		-ExecutionPolicy Bypass
		-File "${RootDir}/Tools/Packaging/Test-PackagedGame.ps1"
		-PackageDir "${HE_GAME_PACKAGE_STAGING_DIR}"
		-ExecutableName "${HE_PROJECT_EXECUTABLE_NAME}"
		-LogDirectory "${CMAKE_CURRENT_BINARY_DIR}/PackageFullStartLogs"
		-TimeoutSeconds 300
		-UseWarp
	COMMAND ${CMAKE_COMMAND}
		-DPACKAGE_OUTPUT_ROOT=${HE_GAME_PACKAGE_OUTPUT_ROOT}
		-DPACKAGE_DIR=${HE_GAME_PACKAGE_DIR}
		-DSTAGING_DIR=${HE_GAME_PACKAGE_STAGING_DIR}
		-DPACKAGE_EXECUTABLE_NAME=${HE_PROJECT_EXECUTABLE_NAME}
		-DPACKAGE_PROJECT_ID=${HE_PROJECT_ID}
		-P "${RootDir}/CMake/PublishGamePackage.cmake"
	VERBATIM
)
endif()

set(HGameHeaders "${GameHeaderFiles}" PARENT_SCOPE)
