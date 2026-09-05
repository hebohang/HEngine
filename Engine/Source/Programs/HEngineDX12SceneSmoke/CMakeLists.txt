ADD_PROGRAM(HEngineDX12SceneSmoke)

target_sources(${TARGET_NAME} PRIVATE
    Main.cpp
)

target_include_directories(${TARGET_NAME} PRIVATE
    "${HE_GENERATED_SOURCE_DIR}"
    "${GeneratedSeriFileDir}"
    "${EngineSourceDir}"
    "${EngineRuntimeDir}"
)

target_link_libraries(${TARGET_NAME} PRIVATE HEngineRuntime nvrhi)
he_attach_static_plugins(${TARGET_NAME} RenderSmoke)
get_target_property(HE_SCENE_SMOKE_PLUGIN_IDS
    ${TARGET_NAME} HE_STATIC_PLUGIN_IDS)
list(FIND HE_SCENE_SMOKE_PLUGIN_IDS
    "he.offline-reference-renderer" HE_SCENE_SMOKE_OFFLINE_INDEX)
if(HE_SCENE_SMOKE_OFFLINE_INDEX EQUAL -1)
    target_compile_definitions(${TARGET_NAME} PRIVATE
        HE_WITH_OFFLINE_REFERENCE_RENDERER_SMOKE=0)
else()
    target_compile_definitions(${TARGET_NAME} PRIVATE
        HE_WITH_OFFLINE_REFERENCE_RENDERER_SMOKE=1)
endif()
set_target_properties(${TARGET_NAME} PROPERTIES CXX_STANDARD 20)

# HShaderCompiler owns the single authoritative runtime shader generation edge.
# Reusing it here prevents the smoke target and HGame/HEngineEditor from running
# separate HShaderCompiler processes against the same DXIL/cachekey files.
add_custom_target(HEffekseerDepthSnapshotShaders)
add_dependencies(HEffekseerDepthSnapshotShaders HEngineRuntimeShaders)

add_custom_target(HEngineDX12SceneSmokeShaders)
add_dependencies(HEngineDX12SceneSmokeShaders HEngineRuntimeShaders)

add_dependencies(${TARGET_NAME}
    HEffekseerDepthSnapshotShaders
    HEngineDX12SceneSmokeShaders)
