set(heLightTarget HEngineWeChat)
if(HE_BUILD_WEB)
    set(heLightTarget HEngineWeb)
endif()
option(HE_SCRIPT_CSHARP_WASM "Enable the real C# plugin in the portable Wasm host" ${HE_BUILD_WEB})
# Host tools are built by hectl using native Windows CMake, never this toolchain.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(HE_WECHAT_CODEGEN_ROOT "" CACHE PATH "Validated Windows DevGame codegen output")
set(HE_WECHAT_SHADER_ROOT "" CACHE PATH "Offline HShaderCompiler WeChat output")
set(HE_WECHAT_DEPENDENCIES "" CACHE PATH "Pinned Sokol / Emscripten downloads")
if(NOT EXISTS "${HE_WECHAT_CODEGEN_ROOT}/.he-codegen/manifest.json")
    message(FATAL_ERROR "Run hectl codegen run -CodeGenHost DevGame before configuring WeChat")
endif()
set(heRuntime "${CMAKE_SOURCE_DIR}/Engine/Source/Runtime")
set(heThirdParty "${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty")
set(HE_PROJECT_DIR "${CMAKE_SOURCE_DIR}/Engine/Templates/Blank" CACHE PATH "HEngine project directory or Project.heproject descriptor.")
get_filename_component(hePortableProject "${HE_PROJECT_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
if(NOT IS_DIRECTORY "${hePortableProject}")
    get_filename_component(hePortableProject "${hePortableProject}" DIRECTORY)
endif()
if(NOT EXISTS "${hePortableProject}/Source/Game/GameLayer/TouchSample.cpp")
    message(FATAL_ERROR "The legacy native portable contract requires a TouchSample project; pass HE_PROJECT_DIR explicitly. C# SDK exports use Tools/Build/Publish.ps1.")
endif()
set(heSources
    ${heRuntime}/Core/UUID.cpp
    ${heRuntime}/Core/Log/Log.cpp
    ${heRuntime}/Core/Meta/Reflection/Reflection.cpp
    ${heRuntime}/Core/Meta/Serializer/Serializer.cpp
    ${heRuntime}/Utils/PathUtils/PathUtils.cpp
    ${heRuntime}/Utils/JsonUtils/JsonUtils.cpp
    ${heRuntime}/Resource/RuntimeResourceCatalog/RuntimeResourceCatalog.cpp
    ${heRuntime}/Resource/RuntimeResourceRegistry/RuntimeResourceRegistry.cpp
    ${heRuntime}/EcsFramework/Serializer/SceneDocument.cpp
    ${heRuntime}/EcsFramework/Entity/Entity.cpp
    ${heRuntime}/EcsFramework/Component/Script/NativeScriptComponent.cpp
    ${heRuntime}/EcsFramework/System/Script/NativeScriptSystem.cpp
    ${heRuntime}/Platform/WeChat/LightLevel.cpp
    ${heRuntime}/Platform/WeChat/Reflection.cpp
    ${heRuntime}/Platform/WeChat/Runtime.cpp
    ${heThirdParty}/crossguid/Guid.cpp
    ${hePortableProject}/Source/Game/GameLayer/TouchSample.cpp)
if(EMSCRIPTEN)
    add_executable(${heLightTarget} ${heSources} ${heRuntime}/Platform/WeChat/SokolRenderer.cpp)
    set_target_properties(${heLightTarget} PROPERTIES OUTPUT_NAME hegame SUFFIX .js)
    target_compile_options(${heLightTarget} PRIVATE -O2 -fexceptions -ffunction-sections -fdata-sections
        -mno-bulk-memory -mno-sign-ext -mno-nontrapping-fptoint -mno-simd128 -mno-atomics)
    target_link_options(${heLightTarget} PRIVATE -O2 -fexceptions --no-entry --profiling-funcs
        -mno-bulk-memory -mno-sign-ext -mno-nontrapping-fptoint -mno-simd128 -mno-atomics
        "-sDISABLE_EXCEPTION_CATCHING=0" "-sWASM_BIGINT=0"
        "-sMODULARIZE=1" "-sEXPORT_NAME=createHEngine" "-sENVIRONMENT=web,shell"
        "-sMIN_WEBGL_VERSION=2" "-sMAX_WEBGL_VERSION=2" "-sGL_PREINITIALIZED_CONTEXT=1"
        "-sINITIAL_MEMORY=33554432" "-sMAXIMUM_MEMORY=134217728" "-sALLOW_MEMORY_GROWTH=1"
        # SceneDocument's bounded reader uses a 64 KiB stack chunk; the
        # Emscripten 64 KiB default cannot hold it plus the calling frames.
        "-sSTACK_SIZE=1048576" "-sSTACK_OVERFLOW_CHECK=2"
        "-sFORCE_FILESYSTEM=1" "-sASSERTIONS=1"
        "-sEXPORTED_FUNCTIONS=['_malloc','_free']"
        "-sEXPORTED_RUNTIME_METHODS=['FS','UTF8ToString','stringToNewUTF8','HEAPU8','HEAPF64']")
else()
    add_executable(${heLightTarget} ${heSources} ${heRuntime}/Platform/WeChat/ContractTests.cpp)
    target_compile_definitions(${heLightTarget} PRIVATE GUID_WINDOWS SPDLOG_WCHAR_FILENAMES NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(${heLightTarget} PRIVATE ole32)
    target_compile_options(${heLightTarget} PRIVATE /utf-8 /bigobj /O2 /Gy)
    target_link_options(${heLightTarget} PRIVATE /OPT:REF /OPT:ICF)
endif()
target_compile_definitions(${heLightTarget} PRIVATE HE_WECHAT=1 WITH_EDITOR=0 GLM_ENABLE_EXPERIMENTAL)
target_include_directories(${heLightTarget} PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine/Source ${heRuntime} ${hePortableProject}/Source
    ${heThirdParty} ${heThirdParty}/glm ${heThirdParty}/entt/include
    ${heThirdParty}/json/include ${heThirdParty}/spdlog/include ${heThirdParty}/stb_image
    ${HE_WECHAT_CODEGEN_ROOT} ${HE_WECHAT_SHADER_ROOT} ${HE_WECHAT_DEPENDENCIES})

if(HE_SCRIPT_CSHARP_WASM)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "The portable C# backend requires the Emscripten target. Use the normal PC target for CoreCLR.")
    endif()
    set(EngineSourceDir "${CMAKE_SOURCE_DIR}/Engine/Source")
    set(EngineRuntimeDir "${heRuntime}")
    set(HE_PLUGIN_CONFIG "${hePortableProject}/Config/Plugins.Portable.json" CACHE FILEPATH "Portable plugin enablement")
    set(hePlatform WeChat)
    if(HE_BUILD_WEB)
        set(hePlatform Web)
        target_compile_definitions(${heLightTarget} PRIVATE HE_WEB=1)
    endif()
    set(heScriptApiRoot "${CMAKE_CURRENT_BINARY_DIR}/generated/scripting")
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DSCHEMA=${CMAKE_SOURCE_DIR}/Tools/Scripting/Api.schema.json"
        "-DOUTPUT_DIR=${heScriptApiRoot}" -P "${CMAKE_SOURCE_DIR}/Tools/Scripting/GenerateApi.cmake"
        COMMAND_ERROR_IS_FATAL ANY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/Tools/Scripting/Api.schema.json" "${CMAKE_SOURCE_DIR}/Tools/Scripting/GenerateApi.cmake")
    target_include_directories(${heLightTarget} PRIVATE "${heScriptApiRoot}")
    target_compile_definitions(${heLightTarget} PRIVATE HE_SCRIPT_CSHARP_WASM=1)
    add_library(HEnginePortableOptions INTERFACE)
    foreach(property IN ITEMS INCLUDE_DIRECTORIES COMPILE_OPTIONS COMPILE_DEFINITIONS)
        get_target_property(value ${heLightTarget} ${property})
        set_property(TARGET HEnginePortableOptions PROPERTY INTERFACE_${property} "${value}")
    endforeach()
    include("${CMAKE_SOURCE_DIR}/CMake/ScriptCore.cmake")
    target_link_libraries(HEngineScriptCore PRIVATE HEnginePortableOptions)
    target_link_libraries(${heLightTarget} PRIVATE HEngineScriptCore)
    target_sources(${heLightTarget} PRIVATE
        ${heRuntime}/Core/Plugins/PluginDescriptor.cpp
        ${heRuntime}/Core/Plugins/PluginManager.cpp
        ${heRuntime}/Core/Plugins/PluginServiceRegistry.cpp
        ${heRuntime}/Camera/SceneCamera.cpp
        ${heRuntime}/Scripting/ScriptWorld.cpp
        ${heRuntime}/Scripting/ScriptEngineApi.cpp
        ${heRuntime}/EcsFramework/System/Script/ScriptSystem.cpp
        ${heRuntime}/Platform/Scripting/WasmScriptServices.cpp)
    include("${CMAKE_SOURCE_DIR}/CMake/HEnginePlugins.cmake")
    he_discover_static_plugins(ROOT "${EngineSourceDir}/Plugins" CONFIG "${HE_PLUGIN_CONFIG}"
        GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/plugins" ACTIVE_HOSTS Game
        PLATFORM ${hePlatform} BUILD_DISTRIBUTION Shipping)
    foreach(pluginId IN LISTS HE_ENABLED_STATIC_PLUGIN_IDS)
        if(NOT pluginId STREQUAL "he.script.csharp")
            message(FATAL_ERROR "The portable rendering profile has no implementation for plugin '${pluginId}'")
        endif()
    endforeach()
    he_add_discovered_static_plugins()
    he_attach_static_plugins(${heLightTarget} Game)
endif()
