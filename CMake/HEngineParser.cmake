# Reflection/serialization generation is a build-graph node. The parser tool
# remains independent from Runtime so the dependency direction is always:
# HEngineParser -> HEngineCodeGen -> generated consumers.

set(HENGINE_PRECOMPILE_PARAMS_IN_PATH "${CMAKE_SOURCE_DIR}/CMake/precompile.json.in")
set(HE_CODEGEN_STATE_DIR "${CMAKE_BINARY_DIR}/codegen")
file(MAKE_DIRECTORY "${HE_CODEGEN_STATE_DIR}")

set(HEProjectHeaders)
if(IS_DIRECTORY "${ProjectRootDir}/Source")
    file(GLOB_RECURSE HEProjectHeaders CONFIGURE_DEPENDS
        "${ProjectRootDir}/Source/*.h"
        "${ProjectRootDir}/Source/*.hpp")
endif()

set(HECodeGenHeaders
    ${GlmHeaderToParse}
    ${HEngineRuntimeHeaders}
    ${HEngineEditorHeaders}
    ${HEnginePluginsHeaders}
    ${HEProjectHeaders})
file(GLOB_RECURSE HECodeGenRuntimeCandidateHeaders CONFIGURE_DEPENDS
    "${EngineRuntimeDir}/*.h"
    "${EngineRuntimeDir}/*.hpp")
list(APPEND HECodeGenHeaders ${HECodeGenRuntimeCandidateHeaders})
list(REMOVE_DUPLICATES HECodeGenHeaders)
list(SORT HECodeGenHeaders)

set(HECodeGenInputHeaders)
foreach(codegenHeader IN LISTS HECodeGenHeaders)
    if(EXISTS "${codegenHeader}")
        file(STRINGS "${codegenHeader}" reflectedTypeMarker
            REGEX "^[ \t]*REFLECTION_TYPE[ \t]*\\(" LIMIT_COUNT 1)
        if(reflectedTypeMarker)
            list(APPEND HECodeGenInputHeaders "${codegenHeader}")
        endif()
    endif()
endforeach()
list(REMOVE_DUPLICATES HECodeGenInputHeaders)
list(SORT HECodeGenInputHeaders)
if(NOT HECodeGenInputHeaders)
    message(FATAL_ERROR "No reflected headers were discovered for HEngineCodeGen.")
endif()

set(HENGINE_PRECOMPILE_PARAMS_PATH "${HE_CODEGEN_STATE_DIR}/input-headers.txt")
configure_file(
    "${HENGINE_PRECOMPILE_PARAMS_IN_PATH}"
    "${HENGINE_PRECOMPILE_PARAMS_PATH}"
    @ONLY)

set(HE_CODEGEN_VALID_HOSTS Editor DevGame Tests Package)
if(HE_CODEGEN_HOST_OVERRIDE)
    list(FIND HE_CODEGEN_VALID_HOSTS "${HE_CODEGEN_HOST_OVERRIDE}"
        HE_CODEGEN_HOST_INDEX)
    if(HE_CODEGEN_HOST_INDEX EQUAL -1)
        message(FATAL_ERROR
            "HE_CODEGEN_HOST_OVERRIDE must be Editor, DevGame, Tests, or Package: ${HE_CODEGEN_HOST_OVERRIDE}")
    endif()
    set(HE_CODEGEN_HOST "${HE_CODEGEN_HOST_OVERRIDE}")
elseif(PACK_GAME)
    set(HE_CODEGEN_HOST "Package")
elseif(HE_BUILD_TESTS)
    set(HE_CODEGEN_HOST "Tests")
elseif(HE_BUILD_DEV_GAME)
    set(HE_CODEGEN_HOST "DevGame")
else()
    set(HE_CODEGEN_HOST "Editor")
endif()

if(PACK_GAME AND NOT HE_CODEGEN_HOST STREQUAL "Package")
    message(FATAL_ERROR "PACK_GAME requires the Package codegen host.")
elseif(HE_CODEGEN_HOST STREQUAL "Package" AND NOT PACK_GAME)
    message(FATAL_ERROR "The Package codegen host requires PACK_GAME=ON.")
elseif((HE_CODEGEN_HOST STREQUAL "DevGame" OR HE_CODEGEN_HOST STREQUAL "Tests")
       AND NOT HE_BUILD_DEV_GAME)
    message(FATAL_ERROR "The DevGame and Tests codegen hosts require HE_BUILD_DEV_GAME=ON.")
elseif(HE_BUILD_TESTS AND NOT HE_CODEGEN_HOST STREQUAL "Tests")
    message(FATAL_ERROR "HE_BUILD_TESTS requires the Tests codegen host.")
elseif(HE_CODEGEN_HOST STREQUAL "Tests" AND NOT HE_BUILD_TESTS)
    message(FATAL_ERROR "The Tests codegen host requires HE_BUILD_TESTS=ON.")
elseif(HE_CODEGEN_HOST STREQUAL "Editor" AND (PACK_GAME OR HE_BUILD_DEV_GAME))
    message(FATAL_ERROR "The Editor codegen host requires the editor build mode.")
endif()

string(JOIN "," HE_CODEGEN_ACTIVE_HOSTS ${HE_STATIC_PLUGIN_ACTIVE_HOSTS})
string(JOIN "," HE_CODEGEN_PLUGIN_IDS ${HE_ENABLED_STATIC_PLUGIN_IDS})
if(NOT HE_CODEGEN_PLUGIN_IDS)
    set(HE_CODEGEN_PLUGIN_IDS "<none>")
endif()
set(HE_CODEGEN_TOOLSET_ID "${CMAKE_GENERATOR_TOOLSET}")
if(NOT HE_CODEGEN_TOOLSET_ID AND MSVC)
    set(HE_CODEGEN_TOOLSET_ID "v143-$ENV{VCToolsVersion}")
elseif(NOT HE_CODEGEN_TOOLSET_ID)
    set(HE_CODEGEN_TOOLSET_ID "native")
endif()

set(HE_CODEGEN_TRACKED_CONFIGS
    "${ProjectDescriptorFile}"
    "${CMAKE_SOURCE_DIR}/CMake/HEngineParser.cmake"
    "${CMAKE_SOURCE_DIR}/CMake/HEnginePlugins.cmake"
    "${CMAKE_SOURCE_DIR}/Engine/Source/CMakeLists.txt")
if(IS_DIRECTORY "${ProjectConfigDir}")
    file(GLOB_RECURSE HE_CODEGEN_PROJECT_CONFIGS CONFIGURE_DEPENDS
        "${ProjectConfigDir}/*.json")
    list(APPEND HE_CODEGEN_TRACKED_CONFIGS ${HE_CODEGEN_PROJECT_CONFIGS})
endif()
file(GLOB_RECURSE HE_CODEGEN_PLUGIN_MANIFESTS CONFIGURE_DEPENDS
    "${EnginePluginsDir}/*/plugin.json")
list(APPEND HE_CODEGEN_TRACKED_CONFIGS ${HE_CODEGEN_PLUGIN_MANIFESTS})
list(REMOVE_DUPLICATES HE_CODEGEN_TRACKED_CONFIGS)
list(SORT HE_CODEGEN_TRACKED_CONFIGS)

string(CONCAT HE_CODEGEN_CONTEXT_CONTENT
    "schemaVersion=1\n"
    "host=${HE_CODEGEN_HOST}\n"
    "configuration=$<CONFIG>\n"
    "module=HEngine\n"
    "platform=${CMAKE_SYSTEM_NAME}\n"
    "generator=${CMAKE_GENERATOR}\n"
    "compilerId=${CMAKE_CXX_COMPILER_ID}\n"
    "compilerVersion=${CMAKE_CXX_COMPILER_VERSION}\n"
    "toolset=${HE_CODEGEN_TOOLSET_ID}\n"
    "$<$<CONFIG:Debug>:define=_DEBUG\n>"
    "$<$<NOT:$<CONFIG:Debug>>:define=NDEBUG\n>"
    "packGame=${PACK_GAME}\n"
    "devGame=${HE_BUILD_DEV_GAME}\n"
    "buildTests=${HE_BUILD_TESTS}\n"
    "enableGM=${HE_ENABLE_GM}\n"
    "distribution=${HE_STATIC_PLUGIN_BUILD_DISTRIBUTION}\n"
    "activeHosts=${HE_CODEGEN_ACTIVE_HOSTS}\n"
    "pluginIds=${HE_CODEGEN_PLUGIN_IDS}\n"
    "sourceRoot=${EngineSourceDir}|engine\n"
    "include=${EngineSourceDir}\n"
    "track=${CMAKE_SOURCE_DIR}/Engine/Source/Programs/HEngineParser/Parser\n"
    "track=${CMAKE_SOURCE_DIR}/Engine/Source/Programs/HEngineParser/CMakeLists.txt\n"
    "arg=-fms-extensions\n"
    "arg=-fms-compatibility\n"
    "arg=-fdelayed-template-parsing\n")

if(IS_DIRECTORY "${ProjectRootDir}/Source")
    string(APPEND HE_CODEGEN_CONTEXT_CONTENT
        "sourceRoot=${ProjectRootDir}/Source|project\n"
        "include=${ProjectRootDir}/Source\n")
endif()
if(IS_DIRECTORY "${ProjectRootDir}/Source/Game")
    string(APPEND HE_CODEGEN_CONTEXT_CONTENT
        "include=${ProjectRootDir}/Source/Game\n")
endif()

if(TARGET HEngineRuntime)
    string(APPEND HE_CODEGEN_CONTEXT_CONTENT
        "include=$<JOIN:$<TARGET_PROPERTY:HEngineRuntime,INCLUDE_DIRECTORIES>,\ninclude=>\n"
        "define=$<JOIN:$<TARGET_PROPERTY:HEngineRuntime,COMPILE_DEFINITIONS>,\ndefine=>\n")
endif()
if(HE_CODEGEN_HOST STREQUAL "Editor")
    if(NOT TARGET HEngineEditor)
        message(FATAL_ERROR "The Editor codegen host requires HEngineEditor.")
    endif()
    string(APPEND HE_CODEGEN_CONTEXT_CONTENT
        "include=$<JOIN:$<TARGET_PROPERTY:HEngineEditor,INCLUDE_DIRECTORIES>,\ninclude=>\n"
        "define=$<JOIN:$<TARGET_PROPERTY:HEngineEditor,COMPILE_DEFINITIONS>,\ndefine=>\n")
elseif(HE_CODEGEN_HOST STREQUAL "Tests")
    if(NOT TARGET HEngineTests)
        message(FATAL_ERROR "The Tests codegen host requires HEngineTests.")
    endif()
    string(APPEND HE_CODEGEN_CONTEXT_CONTENT
        "include=$<JOIN:$<TARGET_PROPERTY:HEngineTests,INCLUDE_DIRECTORIES>,\ninclude=>\n"
        "define=$<JOIN:$<TARGET_PROPERTY:HEngineTests,COMPILE_DEFINITIONS>,\ndefine=>\n")
else()
    if(NOT TARGET HGame)
        message(FATAL_ERROR "The DevGame and Package codegen hosts require HGame.")
    endif()
    string(APPEND HE_CODEGEN_CONTEXT_CONTENT
        "include=$<JOIN:$<TARGET_PROPERTY:HGame,INCLUDE_DIRECTORIES>,\ninclude=>\n"
        "define=$<JOIN:$<TARGET_PROPERTY:HGame,COMPILE_DEFINITIONS>,\ndefine=>\n")
endif()

foreach(trackedConfig IN LISTS HE_CODEGEN_TRACKED_CONFIGS)
    if(EXISTS "${trackedConfig}")
        string(APPEND HE_CODEGEN_CONTEXT_CONTENT "track=${trackedConfig}\n")
    endif()
endforeach()
foreach(trackedHeader IN LISTS HECodeGenHeaders)
    if(EXISTS "${trackedHeader}")
        string(APPEND HE_CODEGEN_CONTEXT_CONTENT "track=${trackedHeader}\n")
    endif()
endforeach()

set(HE_CODEGEN_CONTEXT_PATH "${HE_CODEGEN_STATE_DIR}/context-$<CONFIG>.txt")
file(GENERATE
    OUTPUT "${HE_CODEGEN_CONTEXT_PATH}"
    CONTENT "${HE_CODEGEN_CONTEXT_CONTENT}")

if(HE_CODEGEN_PARSER_EXECUTABLE)
    get_filename_component(HE_CODEGEN_PARSER_COMMAND
        "${HE_CODEGEN_PARSER_EXECUTABLE}" ABSOLUTE BASE_DIR "${CMAKE_BINARY_DIR}")
    if(NOT EXISTS "${HE_CODEGEN_PARSER_COMMAND}")
        message(FATAL_ERROR
            "HE_CODEGEN_PARSER_EXECUTABLE does not exist: ${HE_CODEGEN_PARSER_COMMAND}")
    endif()
else()
    if(NOT TARGET HEngineParser)
        message(FATAL_ERROR "HEngineParser target must exist before codegen integration.")
    endif()
    set(HE_CODEGEN_PARSER_COMMAND "$<TARGET_FILE:HEngineParser>")
endif()

find_program(HE_CODEGEN_POWERSHELL NAMES pwsh powershell REQUIRED)
set(HE_CODEGEN_DRIVER "${CMAKE_SOURCE_DIR}/Tools/Agent/CodeGenDriver.ps1")
set(HE_CODEGEN_MANIFEST "${HE_GENERATED_SOURCE_DIR}/.he-codegen/manifest.json")
string(SHA256 HE_CODEGEN_STATE_HASH
    "${CMAKE_BINARY_DIR};${ProjectRootDir};${HE_GENERATED_SOURCE_BASE_DIR}")
string(SUBSTRING "${HE_CODEGEN_STATE_HASH}" 0 12 HE_CODEGEN_STATE_HASH_SHORT)
set(HE_CODEGEN_LOG
    "${CMAKE_SOURCE_DIR}/out/agent/logs/codegen-$<CONFIG>-${HE_CODEGEN_HOST}-${HE_CODEGEN_STATE_HASH_SHORT}.log")
set(HE_CODEGEN_RESULT "${HE_CODEGEN_STATE_DIR}/result-$<CONFIG>.json")
set(HE_CODEGEN_VERIFY_RESULT "${HE_CODEGEN_STATE_DIR}/verify-result-$<CONFIG>.json")
set(HE_CODEGEN_CONTENT_STAMP "${HE_CODEGEN_STATE_DIR}/${CMAKE_CFG_INTDIR}/content.stamp")
set(HE_CODEGEN_EXECUTION_STAMP "${HE_CODEGEN_STATE_DIR}/${CMAKE_CFG_INTDIR}/inputs.stamp")
file(GLOB HE_CODEGEN_TEMPLATE_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Engine/Source/Programs/HEngineParser/Template/*.mustache")
if(HE_CODEGEN_PARSER_EXECUTABLE)
    set(HE_CODEGEN_TOOL_DEPENDENCY "${HE_CODEGEN_PARSER_COMMAND}")
else()
    set(HE_CODEGEN_TOOL_DEPENDENCY HEngineParser)
endif()

add_custom_command(
    OUTPUT "${HE_CODEGEN_EXECUTION_STAMP}"
    COMMAND "${HE_CODEGEN_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${HE_CODEGEN_DRIVER}"
        -Action Ensure
        -RepositoryRoot "${CMAKE_SOURCE_DIR}"
        -ProjectRoot "${ProjectRootDir}"
        -OutputRoot "${HE_GENERATED_SOURCE_DIR}"
        -ParserExecutable "${HE_CODEGEN_PARSER_COMMAND}"
        -InputList "${HENGINE_PRECOMPILE_PARAMS_PATH}"
        -ContextFile "${HE_CODEGEN_CONTEXT_PATH}"
        -TemplateRoot "${CMAKE_SOURCE_DIR}/Engine/Source/Programs/HEngineParser/Template"
        -Module HEngine
        -LogPath "${HE_CODEGEN_LOG}"
        -ResultPath "${HE_CODEGEN_RESULT}"
        -StampPath "${HE_CODEGEN_CONTENT_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${HE_CODEGEN_EXECUTION_STAMP}"
    DEPENDS
        ${HE_CODEGEN_TOOL_DEPENDENCY}
        "${HE_CODEGEN_DRIVER}"
        "${HENGINE_PRECOMPILE_PARAMS_PATH}"
        "${HE_CODEGEN_CONTEXT_PATH}"
        ${HE_CODEGEN_TEMPLATE_FILES}
        ${HECodeGenHeaders}
        ${HE_CODEGEN_TRACKED_CONFIGS}
    BYPRODUCTS
        "${HE_CODEGEN_CONTENT_STAMP}"
    COMMENT "Checking reflection code generation freshness"
    VERBATIM)

add_custom_target(HEngineCodeGen
    COMMAND "${HE_CODEGEN_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${HE_CODEGEN_DRIVER}"
        -Action Ensure
        -RepositoryRoot "${CMAKE_SOURCE_DIR}"
        -ProjectRoot "${ProjectRootDir}"
        -OutputRoot "${HE_GENERATED_SOURCE_DIR}"
        -ParserExecutable "${HE_CODEGEN_PARSER_COMMAND}"
        -InputList "${HENGINE_PRECOMPILE_PARAMS_PATH}"
        -ContextFile "${HE_CODEGEN_CONTEXT_PATH}"
        -TemplateRoot "${CMAKE_SOURCE_DIR}/Engine/Source/Programs/HEngineParser/Template"
        -Module HEngine
        -LogPath "${HE_CODEGEN_LOG}"
        -StampPath "${HE_CODEGEN_CONTENT_STAMP}"
        -ResultPath "${HE_CODEGEN_VERIFY_RESULT}"
        -Quiet
    DEPENDS "${HE_CODEGEN_EXECUTION_STAMP}"
    SOURCES
        "${HE_CODEGEN_DRIVER}"
        "${HENGINE_PRECOMPILE_PARAMS_PATH}"
        ${HECodeGenHeaders}
        ${HE_CODEGEN_TRACKED_CONFIGS}
    VERBATIM)

if(NOT HE_CODEGEN_PARSER_EXECUTABLE)
    add_dependencies(HEngineCodeGen HEngineParser)
endif()
set_target_properties(HEngineCodeGen PROPERTIES FOLDER "Engine/Generated")

set(HE_CODEGEN_CONSUMERS
    HEngineRuntime
    HEngineEditor
    HGame
    HEngineTests
    HEngineDevAgentRuntime
    HEngineDX12SceneSmoke
    HEngineRmlDX12Smoke
    ${PluginNames})
list(REMOVE_DUPLICATES HE_CODEGEN_CONSUMERS)
set(HE_CODEGEN_OBJECT_DEPENDENCY_COUNT 0)
foreach(codegenConsumer IN LISTS HE_CODEGEN_CONSUMERS)
    if(TARGET "${codegenConsumer}" AND NOT "${codegenConsumer}" STREQUAL "HEngineParser")
        add_dependencies("${codegenConsumer}" HEngineCodeGen)
        get_target_property(codegenConsumerSourceDir "${codegenConsumer}" SOURCE_DIR)
        get_target_property(codegenConsumerSources "${codegenConsumer}" SOURCES)
        foreach(codegenConsumerSource IN LISTS codegenConsumerSources)
            if(codegenConsumerSource MATCHES "^\\$<" OR
               NOT codegenConsumerSource MATCHES "\\.(c|cc|cpp|cxx)$")
                continue()
            endif()
            if(IS_ABSOLUTE "${codegenConsumerSource}")
                set(codegenConsumerSourcePath "${codegenConsumerSource}")
            else()
                cmake_path(ABSOLUTE_PATH codegenConsumerSource
                    BASE_DIRECTORY "${codegenConsumerSourceDir}"
                    NORMALIZE OUTPUT_VARIABLE codegenConsumerSourcePath)
            endif()
            if(EXISTS "${codegenConsumerSourcePath}")
                file(READ "${codegenConsumerSourcePath}" codegenConsumerSourceText)
                string(FIND "${codegenConsumerSourceText}" "_generated/serializer/" generatedSerializerInclude)
                string(FIND "${codegenConsumerSourceText}" ".gen.h" generatedHeaderInclude)
                string(FIND "${codegenConsumerSourceText}" "all_serializer" generatedAggregateInclude)
                if(NOT generatedSerializerInclude EQUAL -1 OR
                   NOT generatedHeaderInclude EQUAL -1 OR
                   NOT generatedAggregateInclude EQUAL -1)
                    set_property(SOURCE "${codegenConsumerSourcePath}"
                        TARGET_DIRECTORY "${codegenConsumer}"
                        APPEND PROPERTY OBJECT_DEPENDS "${HE_CODEGEN_CONTENT_STAMP}")
                    math(EXPR HE_CODEGEN_OBJECT_DEPENDENCY_COUNT
                        "${HE_CODEGEN_OBJECT_DEPENDENCY_COUNT} + 1")
                endif()
            endif()
        endforeach()
    endif()
endforeach()
message(STATUS
    "HEngineCodeGen content stamp tracks ${HE_CODEGEN_OBJECT_DEPENDENCY_COUNT} direct generated consumers.")

if(CPP_CODE_GEN)
    message(WARNING
        "CPP_CODE_GEN no longer runs generation during configure; build HEngineCodeGen instead.")
endif()
