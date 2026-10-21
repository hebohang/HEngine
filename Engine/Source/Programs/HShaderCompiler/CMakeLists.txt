ADD_PROGRAM(HShaderCompiler)
target_compile_definitions(HShaderCompiler PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(HShaderCompiler PRIVATE /utf-8)
endif()

if(APPLE)
    set(heShaderFileSystem "${EngineRuntimeDir}/Platform/macOS/MacPlatformFileSystem.cpp")
else()
    set(heShaderFileSystem "${EngineRuntimeDir}/Platform/Windows/WindowsPlatformFileSystem.cpp")
endif()

set(src
    "${CMAKE_CURRENT_SOURCE_DIR}/Main.cpp"
    "${EngineRuntimeDir}/Graphics/Material/MaterialShaderManifest.h"
    "${EngineRuntimeDir}/Graphics/Material/MaterialShaderManifest.cpp"
    "${EngineRuntimeDir}/Graphics/RHI/ShaderArtifact.h"
    "${EngineRuntimeDir}/Platform/PlatformFileSystem.h"
    "${heShaderFileSystem}"
)

target_sources(${TARGET_NAME} PRIVATE ${src})

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/Main.cpp")
source_group("Runtime/Graphics/Material" FILES
    "${EngineRuntimeDir}/Graphics/Material/MaterialShaderManifest.h"
    "${EngineRuntimeDir}/Graphics/Material/MaterialShaderManifest.cpp")
source_group("Runtime/Graphics/RHI" FILES
    "${EngineRuntimeDir}/Graphics/RHI/ShaderArtifact.h")
source_group("Runtime/Platform" FILES
    "${EngineRuntimeDir}/Platform/PlatformFileSystem.h"
    "${heShaderFileSystem}")

target_include_directories(${TARGET_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/cmdline")
target_include_directories(${TARGET_NAME} PRIVATE "${EngineSourceDir}")

# SPIRV-Tools is an offline compiler dependency only. Runtime/Game never link
# it and shipping packages contain neither the validator nor Vulkan SDK tools.
set(heShaderSpirvDefault ${HE_RENDER_VULKAN})
if(HE_SHADER_TOOL_ONLY)
    set(heShaderSpirvDefault ON)
endif()
option(HE_SHADER_SPIRV "Enable offline SPIR-V generation and validation." ${heShaderSpirvDefault})
if(HE_RENDER_VULKAN AND NOT HE_SHADER_SPIRV)
    message(FATAL_ERROR "Vulkan requires SPIR-V shader support.")
endif()
if(HE_SHADER_SPIRV AND NOT TARGET SPIRV-Tools-static)
    # Upstream SPIRV-Tools consumes the legacy PYTHON_EXECUTABLE variable.  On
    # Windows an empty value executes .py files through the user's file
    # association (which may launch an editor and hang an owned build).
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")
    set(SPIRV-Headers_SOURCE_DIR
        "${EngineSourceDir}/ThirdParty/SPIRV-Headers")
    set(SPIRV_SKIP_TESTS ON CACHE BOOL "Skip SPIRV-Tools tests" FORCE)
    set(SPIRV_WERROR OFF CACHE BOOL "Do not promote SPIRV-Tools warnings" FORCE)
    set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "Skip SPIRV-Tools install" FORCE)
    set(SPIRV_TOOLS_BUILD_STATIC ON CACHE BOOL "Build static SPIRV-Tools" FORCE)
    add_subdirectory(
        "${EngineSourceDir}/ThirdParty/SPIRV-Tools"
        "${CMAKE_CURRENT_BINARY_DIR}/SPIRV-Tools"
        EXCLUDE_FROM_ALL)
endif()
target_link_libraries(${TARGET_NAME} PRIVATE
    nlohmann_json::nlohmann_json)
if(HE_SHADER_SPIRV)
    target_link_libraries(${TARGET_NAME} PRIVATE SPIRV-Tools-static)
endif()
target_compile_definitions(${TARGET_NAME} PRIVATE HE_SHADER_SPIRV=$<BOOL:${HE_SHADER_SPIRV}>)

set(HE_DXC_RUNTIME_TARGET "${TARGET_NAME}RuntimeFiles")
if(APPLE)
    find_program(HE_DXC_EXECUTABLE NAMES dxc HINTS "$ENV{VULKAN_SDK}/bin"
        DOC "Native macOS DXC executable from the Vulkan SDK" REQUIRED)
    if(NOT HE_SHADER_SPIRV)
        message(FATAL_ERROR "The native macOS shader compiler requires SPIR-V support.")
    endif()
    target_compile_definitions(${TARGET_NAME} PRIVATE
        HE_DXC_DEFAULT_EXECUTABLE="${HE_DXC_EXECUTABLE}")
    add_custom_target(${HE_DXC_RUNTIME_TARGET}
        COMMAND ${CMAKE_COMMAND}
            "-DHE_DXC_INPUT=${HE_DXC_EXECUTABLE}"
            "-DHE_DXC_OUTPUT=$<TARGET_FILE_DIR:${TARGET_NAME}>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/DeployMacDxc.cmake"
        DEPENDS "${HE_DXC_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/DeployMacDxc.cmake"
        COMMENT "Installing the native macOS DXC runtime"
        VERBATIM)
else()
set(HE_DXC_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/dxc/binary/x64")
if(HE_WINDOWS_ARCH STREQUAL "arm64")
    set(HE_DXC_RUNTIME_DIR "${HE_ARM64_DXC_ROOT}")
endif()
set(HE_DXC_RUNTIME_FILES
    "${HE_DXC_RUNTIME_DIR}/dxc.exe"
    "${HE_DXC_RUNTIME_DIR}/dxcompiler.dll"
    "${HE_DXC_RUNTIME_DIR}/dxil.dll")
add_custom_target(${HE_DXC_RUNTIME_TARGET}
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${TARGET_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${HE_DXC_RUNTIME_DIR}/dxc.exe" "$<TARGET_FILE_DIR:${TARGET_NAME}>/dxc.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${HE_DXC_RUNTIME_DIR}/dxcompiler.dll" "$<TARGET_FILE_DIR:${TARGET_NAME}>/dxcompiler.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${HE_DXC_RUNTIME_DIR}/dxil.dll" "$<TARGET_FILE_DIR:${TARGET_NAME}>/dxil.dll"
    DEPENDS ${HE_DXC_RUNTIME_FILES}
    COMMENT "Installing the HShaderCompiler DXC runtime"
    VERBATIM)
endif()
set_target_properties(${HE_DXC_RUNTIME_TARGET} PROPERTIES
    FOLDER "Programs/HShaderCompiler")
add_dependencies(${TARGET_NAME} ${HE_DXC_RUNTIME_TARGET})

if(HE_SHADER_TOOL_ONLY)
    return()
endif()

if(HE_RENDER_DX12 AND HE_RENDER_VULKAN)
    set(HE_SHADER_BINARY_FORMAT all)
elseif(HE_RENDER_DX12)
    set(HE_SHADER_BINARY_FORMAT dxil)
else()
    set(HE_SHADER_BINARY_FORMAT spirv)
endif()

# Runtime shaders are generated from source and intentionally ignored by Git.
# Keep one authoritative dependency target so Game and Editor never rely on a
# developer's pre-existing DXIL cache. The compiler's content hash still avoids
# redundant DXC work when only an unrelated source changed.
set(HE_RUNTIME_SHADER_SOURCES
    "${EngineContentShadersDir}/Materials/BuiltInShowcase/FresnelRim.hlsl"
    "${EngineContentShadersDir}/Materials/BuiltInShowcase/HologramGlass.hlsl"
    "${EngineContentShadersDir}/Materials/BuiltInShowcase/IridescentLaser.hlsl"
    "${EngineContentShadersDir}/Materials/BuiltInShowcase/MaskedDissolve.hlsl"
    "${EngineContentShadersDir}/Materials/BuiltInShowcase/ProceduralPattern.hlsl"
    "${EngineContentShadersDir}/Materials/BuiltInShowcase/ShowcaseFloor.hlsl"
    "${EngineContentShadersDir}/Materials/BuiltInShowcase/ShowcasePBR.hlsl"
    "${EngineContentShadersDir}/Materials/Character/Skin.hlsl"
    "${EngineContentShadersDir}/Materials/Character/SkinTwoSided.hlsl"
    "${EngineContentShadersDir}/Materials/Character/HairCards.hlsl"
    "${EngineContentShadersDir}/Materials/Character/Cloth.hlsl"
    "${EngineContentShadersDir}/Materials/Character/Eye.hlsl"
    "${EngineContentShadersDir}/Materials/Character/Cornea.hlsl"
    "${EngineContentShadersDir}/Materials/Foliage/Subsurface.hlsl"
    "${EngineContentShadersDir}/GlobalIllumination/SceneSurfaceData.hlsl"
    "${EngineContentShadersDir}/GlobalIllumination/DepthPyramid.hlsl"
    "${EngineContentShadersDir}/GlobalIllumination/RtgiSvgf.hlsl"
    "${EngineContentShadersDir}/GlobalIllumination/ScreenSpaceGlobalIllumination.hlsl"
    "${EngineContentShadersDir}/GlobalIllumination/GlobalIlluminationComposite.hlsl"
    "${EngineContentShadersDir}/RayTracing/HardwareRayTracingRealtime.hlsl"
    "${EngineContentShadersDir}/Modern/EffekseerDepthSnapshot.hlsl"
    "${EngineContentShadersDir}/Modern/RuntimeUiComposite.hlsl"
    "${EngineContentShadersDir}/Modern/SceneDepthPrepass.hlsl"
    "${EngineContentShadersDir}/Modern/SceneDepthPrepassHeapDirect.hlsl"
    "${EngineContentShadersDir}/Modern/SceneEnvironmentPrecompute.hlsl"
    "${EngineContentShadersDir}/Modern/SceneFxaa.hlsl"
    "${EngineContentShadersDir}/Modern/SceneGpuCull.hlsl"
    "${EngineContentShadersDir}/Modern/SceneOpaque.hlsl"
    "${EngineContentShadersDir}/Modern/SceneDeferred.hlsl"
    "${EngineContentShadersDir}/Modern/SceneDeferredSkin.hlsl"
    "${EngineContentShadersDir}/Modern/SceneSkinSubsurface.hlsl"
    "${EngineContentShadersDir}/Modern/SceneGBufferUnpack.hlsl"
    "${EngineContentShadersDir}/Modern/SceneGBufferTaaGuides.hlsl"
    "${EngineContentShadersDir}/Modern/SceneOpaqueHeapDirect.hlsl"
    "${EngineContentShadersDir}/Modern/SceneOutline.hlsl"
    "${EngineContentShadersDir}/Modern/ScenePrimitive2D.hlsl"
    "${EngineContentShadersDir}/Modern/ScenePrimitive2DHeapDirect.hlsl"
    "${EngineContentShadersDir}/Modern/SceneShadow.hlsl"
    "${EngineContentShadersDir}/Modern/SceneShadowHeapDirect.hlsl"
    "${EngineContentShadersDir}/Modern/SceneSky.hlsl"
    "${EngineContentShadersDir}/Modern/SceneSmaa.hlsl"
    "${EngineContentShadersDir}/Modern/SceneToneMap.hlsl"
    "${EngineContentShadersDir}/Modern/ScenePostProcess.hlsl"
    "${EngineContentShadersDir}/Modern/SceneUpscalingInputs.hlsl"
    "${EngineContentShadersDir}/Modern/SceneUpscalingFallback.hlsl"
    "${EngineContentShadersDir}/Modern/SceneUpscalingCoverage.hlsl"
    "${EngineContentShadersDir}/Modern/SceneLightGrid.hlsl"
    "${EngineContentShadersDir}/Modern/SceneCloud.hlsl"
    "${EngineContentShadersDir}/Modern/SceneCloudComposite.hlsl"
    "${EngineContentShadersDir}/Modern/SceneGtao.hlsl"
    "${EngineContentShadersDir}/Modern/SceneReflections.hlsl"
    "${EngineContentShadersDir}/Modern/SceneShadowGpuCull.hlsl"
    "${EngineContentShadersDir}/Modern/SceneGpuOcclusion.hlsl"
    "${EngineContentShadersDir}/Modern/SceneIndirectCompaction.hlsl"
    "${EngineContentShadersDir}/Modern/SceneTaa.hlsl"
    "${EngineContentShadersDir}/Modern/SceneTaaReconstruct.hlsl"
    "${EngineContentShadersDir}/Validation/NvrhiImGui.hlsl"
    "${EngineContentShadersDir}/Validation/NvrhiRml.hlsl"
    "${EngineContentShadersDir}/Validation/NvrhiRmlDecorator.hlsl"
    "${EngineContentShadersDir}/Validation/NvrhiRmlPostprocess.hlsl"
)

# Engine shader logical paths are an internal ABI. Projects may add shaders
# and include the public Engine/Content/Shaders/Include files, but replacing an
# engine-owned source/include would make Editor/Game and packaged DXIL diverge.
file(GLOB_RECURSE HE_ENGINE_SHADER_FILES CONFIGURE_DEPENDS
    RELATIVE "${EngineContentShadersDir}"
    "${EngineContentShadersDir}/*.hlsl"
    "${EngineContentShadersDir}/*.hlsli")
set(HE_ENGINE_SHADER_FILE_KEYS)
set(HE_ENGINE_SHADER_OUTPUT_KEYS)
foreach(engineShaderFile IN LISTS HE_ENGINE_SHADER_FILES)
    string(TOLOWER "${engineShaderFile}" engineShaderFileKey)
    list(APPEND HE_ENGINE_SHADER_FILE_KEYS "${engineShaderFileKey}")
    get_filename_component(engineShaderExtension "${engineShaderFile}" EXT)
    string(TOLOWER "${engineShaderExtension}" engineShaderExtension)
    if(engineShaderExtension STREQUAL ".hlsl")
        file(STRINGS "${EngineContentShadersDir}/${engineShaderFile}"
            engineMaterialPragma
            REGEX "^[ \t]*#[ \t]*pragma[ \t]+hmaterial([ \t]|$)"
            LIMIT_COUNT 1)
        if(NOT engineMaterialPragma)
            get_filename_component(engineShaderStem "${engineShaderFile}" NAME_WE)
            string(TOLOWER "${engineShaderStem}" engineShaderOutputKey)
            list(FIND HE_ENGINE_SHADER_OUTPUT_KEYS "${engineShaderOutputKey}" engineOutputIndex)
            if(NOT engineOutputIndex EQUAL -1)
                message(FATAL_ERROR
                    "Engine legacy shaders have a flat-cache basename collision: ${engineShaderStem}")
            endif()
            list(APPEND HE_ENGINE_SHADER_OUTPUT_KEYS "${engineShaderOutputKey}")
        endif()
    endif()
endforeach()
set(HE_PROJECT_SHADER_SOURCES)
set(HE_PROJECT_SHADER_INCLUDES)
if(IS_DIRECTORY "${ProjectShadersDir}")
    file(GLOB_RECURSE HE_PROJECT_SHADER_SOURCES CONFIGURE_DEPENDS
        "${ProjectShadersDir}/*.hlsl")
    file(GLOB_RECURSE HE_PROJECT_SHADER_INCLUDES CONFIGURE_DEPENDS
        "${ProjectShadersDir}/*.hlsli")
    list(SORT HE_PROJECT_SHADER_SOURCES)
    list(SORT HE_PROJECT_SHADER_INCLUDES)
    set(HE_PROJECT_SHADER_FILES
        ${HE_PROJECT_SHADER_SOURCES}
        ${HE_PROJECT_SHADER_INCLUDES})
    foreach(projectShaderPath IN LISTS HE_PROJECT_SHADER_FILES)
        file(RELATIVE_PATH projectShaderFile
            "${ProjectShadersDir}" "${projectShaderPath}")
        string(TOLOWER "${projectShaderFile}" projectShaderFileKey)
        list(FIND HE_ENGINE_SHADER_FILE_KEYS "${projectShaderFileKey}" engineShaderIndex)
        if(NOT engineShaderIndex EQUAL -1)
            message(FATAL_ERROR
                "Project shader conflicts with an engine-owned logical path: ${projectShaderFile}")
        endif()
        get_filename_component(projectShaderExtension "${projectShaderFile}" EXT)
        string(TOLOWER "${projectShaderExtension}" projectShaderExtension)
        if(projectShaderExtension STREQUAL ".hlsl")
            file(STRINGS "${projectShaderPath}"
                projectMaterialPragma
                REGEX "^[ \t]*#[ \t]*pragma[ \t]+hmaterial([ \t]|$)"
                LIMIT_COUNT 1)
            if(NOT projectMaterialPragma)
                get_filename_component(projectShaderStem "${projectShaderFile}" NAME_WE)
                string(TOLOWER "${projectShaderStem}" projectShaderOutputKey)
                list(FIND HE_ENGINE_SHADER_OUTPUT_KEYS "${projectShaderOutputKey}" engineOutputIndex)
                if(NOT engineOutputIndex EQUAL -1)
                    message(FATAL_ERROR
                        "Project legacy shader basename would overwrite an engine cache artifact: ${projectShaderFile}")
                endif()
                list(FIND HE_PROJECT_SHADER_OUTPUT_KEYS "${projectShaderOutputKey}" projectOutputIndex)
                if(NOT projectOutputIndex EQUAL -1)
                    message(FATAL_ERROR
                        "Project legacy shaders have a flat-cache basename collision: ${projectShaderStem}")
                endif()
                list(APPEND HE_PROJECT_SHADER_OUTPUT_KEYS "${projectShaderOutputKey}")
            endif()
        endif()
    endforeach()
endif()
set_property(GLOBAL PROPERTY HE_RUNTIME_SHADER_SOURCES
    "${HE_RUNTIME_SHADER_SOURCES}")
file(GLOB_RECURSE HE_RUNTIME_SHADER_INCLUDES CONFIGURE_DEPENDS
    "${EngineContentShadersDir}/*.hlsli")

set(HE_RUNTIME_SHADER_COMMANDS)
foreach(shaderSource IN LISTS HE_RUNTIME_SHADER_SOURCES)
    list(APPEND HE_RUNTIME_SHADER_COMMANDS
        COMMAND $<TARGET_FILE:HShaderCompiler>
            --rootPath "${EngineContentShadersDir}"
            --outputPath "${EngineGeneratedShadersDir}"
            --shader "${shaderSource}"
            --binary-format ${HE_SHADER_BINARY_FORMAT})
endforeach()
if(HE_PROJECT_SHADER_SOURCES)
    list(APPEND HE_RUNTIME_SHADER_COMMANDS
        COMMAND $<TARGET_FILE:HShaderCompiler>
            --path "${ProjectShadersDir}"
            --rootPath "${ProjectShadersDir}"
            --outputPath "${EngineGeneratedShadersDir}"
            --includePath "${EngineContentShadersDir}"
            --binary-format ${HE_SHADER_BINARY_FORMAT})
endif()

add_custom_target(HEngineRuntimeShaders
    ${HE_RUNTIME_SHADER_COMMANDS}
    DEPENDS HShaderCompiler
        ${HE_RUNTIME_SHADER_SOURCES}
        ${HE_RUNTIME_SHADER_INCLUDES}
        ${HE_PROJECT_SHADER_SOURCES}
        ${HE_PROJECT_SHADER_INCLUDES}
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:HShaderCompiler>"
    COMMENT "Compiling HEngine runtime HLSL to DXIL and SPIR-V with DXC"
    VERBATIM)
set_target_properties(HEngineRuntimeShaders PROPERTIES FOLDER "Programs/HShaderCompiler")

# Every enabled plugin owns its shader source list and manifest roots. Generate
# isolated build/contract targets without teaching this global tool about a
# concrete plugin or provider type.
get_property(HE_ENABLED_PLUGIN_IDS GLOBAL PROPERTY HE_STATIC_PLUGIN_ENABLED_IDS)
foreach(pluginId IN LISTS HE_ENABLED_PLUGIN_IDS)
    _he_plugin_get(pluginModule "${pluginId}" MODULE_NAME)
    _he_plugin_get(pluginSourceDirectory "${pluginId}" SOURCE_DIR)
    _he_plugin_get(pluginShaderRoots "${pluginId}" SHADER_ROOTS)
    if(NOT TARGET "${pluginModule}")
        message(FATAL_ERROR
            "Enabled plugin '${pluginId}' has no target '${pluginModule}'")
    endif()
    get_target_property(pluginShaderSources
        "${pluginModule}" HE_PLUGIN_SHADER_SOURCES)
    if(pluginShaderSources STREQUAL "pluginShaderSources-NOTFOUND")
        set(pluginShaderSources "")
    endif()
    if(pluginShaderRoots AND NOT pluginShaderSources)
        message(FATAL_ERROR
            "Plugin '${pluginId}' declares ShaderRoots but no HE_PLUGIN_SHADER_SOURCES")
    endif()
    if(pluginShaderSources AND NOT pluginShaderRoots)
        message(FATAL_ERROR
            "Plugin '${pluginId}' declares shader sources but no manifest ShaderRoots")
    endif()
    if(NOT pluginShaderSources)
        continue()
    endif()

    set(pluginShaderOutput
        "${EngineGeneratedContentDir}/Plugins/${pluginModule}/Shaders")
    set(pluginShaderCommands)
    set(pluginShaderContractCommands)
    foreach(pluginShaderSource IN LISTS pluginShaderSources)
        get_filename_component(pluginShaderSource
            "${pluginShaderSource}" ABSOLUTE)
        set(pluginShaderRoot "")
        foreach(pluginShaderRootRelative IN LISTS pluginShaderRoots)
            get_filename_component(candidatePluginShaderRoot
                "${pluginSourceDirectory}/${pluginShaderRootRelative}" ABSOLUTE)
            file(RELATIVE_PATH shaderRelativeToRoot
                "${candidatePluginShaderRoot}" "${pluginShaderSource}")
            if(NOT IS_ABSOLUTE "${shaderRelativeToRoot}" AND
               NOT shaderRelativeToRoot MATCHES "(^|/)\\.\\.(/|$)")
                set(pluginShaderRoot "${candidatePluginShaderRoot}")
                break()
            endif()
        endforeach()
        if(NOT pluginShaderRoot)
            message(FATAL_ERROR
                "Plugin '${pluginId}' shader is outside its manifest ShaderRoots: ${pluginShaderSource}")
        endif()
        list(APPEND pluginShaderCommands
            COMMAND $<TARGET_FILE:HShaderCompiler>
                --rootPath "${pluginShaderRoot}"
                --outputPath "${pluginShaderOutput}"
                --shader "${pluginShaderSource}"
                --includePath "${EngineContentShadersDir}"
                --binary-format ${HE_SHADER_BINARY_FORMAT})
        list(APPEND pluginShaderContractCommands
            COMMAND $<TARGET_FILE:HShaderCompiler>
                --rootPath "${pluginShaderRoot}"
                --outputPath
                    "${CMAKE_CURRENT_BINARY_DIR}/${pluginModule}ShaderContract"
                --shader "${pluginShaderSource}"
                --includePath "${EngineContentShadersDir}"
                --binary-format ${HE_SHADER_BINARY_FORMAT})
    endforeach()

    set(pluginShaderTarget "${pluginModule}Shaders")
    add_custom_target("${pluginShaderTarget}"
        ${pluginShaderCommands}
        DEPENDS HShaderCompiler ${pluginShaderSources}
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:HShaderCompiler>"
        COMMENT "Compiling ${pluginId} plugin shaders"
        VERBATIM)
    set(pluginShaderContractTarget "${pluginModule}ShaderContract")
    add_custom_target("${pluginShaderContractTarget}"
        ${pluginShaderContractCommands}
        DEPENDS HShaderCompiler ${pluginShaderSources}
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:HShaderCompiler>"
        COMMENT "Validating ${pluginId} plugin shader artifacts"
        VERBATIM)
    set_target_properties(
        "${pluginShaderTarget}" "${pluginShaderContractTarget}"
        PROPERTIES FOLDER "Plugins/${pluginModule}")
    set_property(TARGET "${pluginModule}" PROPERTY
        HE_PLUGIN_SHADER_TARGET "${pluginShaderTarget}")
    set_property(TARGET "${pluginModule}" PROPERTY
        HE_PLUGIN_SHADER_CONTRACT_TARGET "${pluginShaderContractTarget}")
endforeach()

add_custom_target(HShaderCompilerSelfTest
    COMMAND $<TARGET_FILE:HShaderCompiler> --self-test
    DEPENDS HShaderCompiler
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:HShaderCompiler>"
    COMMENT "Running HShaderCompiler material ABI and publication self-tests"
    VERBATIM)
set_target_properties(HShaderCompilerSelfTest PROPERTIES FOLDER "Programs/HShaderCompiler")

set(HE_SHADER_ARTIFACT_CONTRACT_SOURCES
    "${EngineContentShadersDir}/Validation/ShaderArtifactContract.hlsl"
    "${EngineContentShadersDir}/GlobalIllumination/DepthPyramid.hlsl"
    "${EngineContentShadersDir}/GlobalIllumination/GlobalIlluminationComposite.hlsl"
    "${EngineContentShadersDir}/GlobalIllumination/RtgiSvgf.hlsl"
    "${EngineContentShadersDir}/GlobalIllumination/SceneSurfaceData.hlsl"
    "${EngineContentShadersDir}/GlobalIllumination/ScreenSpaceGlobalIllumination.hlsl"
    "${EngineContentShadersDir}/Modern/SceneFxaa.hlsl"
    "${EngineContentShadersDir}/Modern/SceneSmaa.hlsl"
    "${EngineContentShadersDir}/Modern/SceneTaa.hlsl"
    "${EngineContentShadersDir}/RayTracing/HardwareRayTracingRealtime.hlsl")
set(HE_SHADER_ARTIFACT_CONTRACT_OUTPUT
    "${CMAKE_CURRENT_BINARY_DIR}/ShaderArtifactContract")
set(HE_SHADER_ARTIFACT_CONTRACT_COMMANDS)
foreach(shaderContractSource IN LISTS HE_SHADER_ARTIFACT_CONTRACT_SOURCES)
    list(APPEND HE_SHADER_ARTIFACT_CONTRACT_COMMANDS
        COMMAND $<TARGET_FILE:HShaderCompiler>
            --rootPath "${EngineContentShadersDir}"
            --outputPath "${HE_SHADER_ARTIFACT_CONTRACT_OUTPUT}"
            --shader "${shaderContractSource}"
            --binary-format ${HE_SHADER_BINARY_FORMAT})
endforeach()
add_custom_target(HShaderArtifactContract
    ${HE_SHADER_ARTIFACT_CONTRACT_COMMANDS}
    COMMAND $<TARGET_FILE:HShaderCompiler> --self-test
    DEPENDS HShaderCompiler ${HE_SHADER_ARTIFACT_CONTRACT_SOURCES}
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:HShaderCompiler>"
    COMMENT "Compiling and validating backend-neutral DXIL/SPIR-V shader artifacts"
    VERBATIM)
set_target_properties(HShaderArtifactContract PROPERTIES FOLDER "Programs/HShaderCompiler")
