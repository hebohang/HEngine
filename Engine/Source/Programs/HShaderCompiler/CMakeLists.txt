ADD_PROGRAM(HShaderCompiler)

set(src
    "${CMAKE_CURRENT_SOURCE_DIR}/Main.cpp"
    "${EngineRuntimeDir}/Graphics/Material/MaterialShaderManifest.h"
    "${EngineRuntimeDir}/Graphics/Material/MaterialShaderManifest.cpp"
    "${EngineRuntimeDir}/Graphics/RHI/ShaderArtifact.h"
    "${EngineRuntimeDir}/Platform/PlatformFileSystem.h"
    "${EngineRuntimeDir}/Platform/Windows/WindowsPlatformFileSystem.cpp"
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
    "${EngineRuntimeDir}/Platform/Windows/WindowsPlatformFileSystem.cpp")

target_include_directories(${TARGET_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/cmdline")
target_include_directories(${TARGET_NAME} PRIVATE "${EngineSourceDir}")

# SPIRV-Tools is an offline compiler dependency only. Runtime/Game never link
# it and shipping packages contain neither the validator nor Vulkan SDK tools.
if(NOT TARGET SPIRV-Tools-static)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    # SPIRV-Tools still invokes the legacy variable in its generated-table
    # commands. Set it explicitly so Windows never falls back to the .py file
    # association (which may be an editor rather than Python).
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
    nlohmann_json::nlohmann_json
    SPIRV-Tools-static)

set(HE_DXC_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/dxc/binary/x64")
set(HE_DXC_RUNTIME_FILES
    "${HE_DXC_RUNTIME_DIR}/dxc.exe"
    "${HE_DXC_RUNTIME_DIR}/dxcompiler.dll"
    "${HE_DXC_RUNTIME_DIR}/dxil.dll")
set(HE_DXC_RUNTIME_TARGET "${TARGET_NAME}RuntimeFiles")
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
set_target_properties(${HE_DXC_RUNTIME_TARGET} PROPERTIES
    FOLDER "Programs/HShaderCompiler")
add_dependencies(${TARGET_NAME} ${HE_DXC_RUNTIME_TARGET})

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
    "${EngineContentShadersDir}/Modern/SceneOpaqueHeapDirect.hlsl"
    "${EngineContentShadersDir}/Modern/SceneOutline.hlsl"
    "${EngineContentShadersDir}/Modern/ScenePrimitive2D.hlsl"
    "${EngineContentShadersDir}/Modern/ScenePrimitive2DHeapDirect.hlsl"
    "${EngineContentShadersDir}/Modern/SceneShadow.hlsl"
    "${EngineContentShadersDir}/Modern/SceneShadowHeapDirect.hlsl"
    "${EngineContentShadersDir}/Modern/SceneSky.hlsl"
    "${EngineContentShadersDir}/Modern/SceneSmaa.hlsl"
    "${EngineContentShadersDir}/Modern/SceneToneMap.hlsl"
    "${EngineContentShadersDir}/Modern/SceneTaa.hlsl"
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
            --binary-format all)
endforeach()
if(HE_PROJECT_SHADER_SOURCES)
    list(APPEND HE_RUNTIME_SHADER_COMMANDS
        COMMAND $<TARGET_FILE:HShaderCompiler>
            --path "${ProjectShadersDir}"
            --rootPath "${ProjectShadersDir}"
            --outputPath "${EngineGeneratedShadersDir}"
            --includePath "${EngineContentShadersDir}"
            --binary-format all)
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
                --binary-format all)
        list(APPEND pluginShaderContractCommands
            COMMAND $<TARGET_FILE:HShaderCompiler>
                --rootPath "${pluginShaderRoot}"
                --outputPath
                    "${CMAKE_CURRENT_BINARY_DIR}/${pluginModule}ShaderContract"
                --shader "${pluginShaderSource}"
                --includePath "${EngineContentShadersDir}"
                --binary-format all)
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
            --binary-format all)
endforeach()
add_custom_target(HShaderArtifactContract
    ${HE_SHADER_ARTIFACT_CONTRACT_COMMANDS}
    COMMAND $<TARGET_FILE:HShaderCompiler> --self-test
    DEPENDS HShaderCompiler ${HE_SHADER_ARTIFACT_CONTRACT_SOURCES}
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:HShaderCompiler>"
    COMMENT "Compiling and validating backend-neutral DXIL/SPIR-V shader artifacts"
    VERBATIM)
set_target_properties(HShaderArtifactContract PROPERTIES FOLDER "Programs/HShaderCompiler")
