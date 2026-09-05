add_executable(HEngineTests
    TestMain.cpp
    AntiAliasingTests.cpp
    AudioLifecycleTests.cpp
    BindlessDescriptorManagerTests.cpp
    ConfigManagerTests.cpp
    DevelopmentLaunchPathsTests.cpp
    CornellSceneSerializationTests.cpp
    DevCommandTests.cpp
    EffectTransformBatchTests.cpp
    EffekseerRegressionManifestTests.cpp
    EffekseerRuntimeLifecycleTests.cpp
    GamepadInputTests.cpp
    HardwareRayTracingTests.cpp
    MaterialHotReloadTests.cpp
    MaterialParameterPackingTests.cpp
    MaterialPipelineKeyTests.cpp
	MaterialResourceCacheTests.cpp
	MaterialThumbnailCacheKeyTests.cpp
    MaterialSerializationTests.cpp
    MeshCpuDecodeTests.cpp
    PluginSystemTests.cpp
    ReflectedResourceSchemaCompatibilityTests.cpp
    RenderCapabilityPolicyTests.cpp
    RenderGraphAccelerationStructureTests.cpp
    RenderGraphTests.cpp
    RenderGraphResourcePoolTests.cpp
    RenderSettingsTests.cpp
    RenderRhiContractTests.cpp
    RenderWorldSubmissionTests.cpp
    ViewportRendererExtensionTests.cpp
    RuntimePrefabResourceTests.cpp
    RuntimeResourceCatalogTests.cpp
    RuntimeResourceRegistryTests.cpp
    RuntimeResourceWarmupTests.cpp
	RuntimeSceneRequestTests.cpp
    RuntimeUiResourceTests.cpp
    RmlUiPointerPayloadTests.cpp
    SceneDocumentTests.cpp
    SystemLifecycleTests.cpp
)

target_include_directories(HEngineTests PRIVATE
    "${EngineSourceDir}"
    "${EngineRuntimeDir}"
)
target_link_libraries(HEngineTests PRIVATE HEngineRuntime)
he_attach_static_plugins(HEngineTests Tests)
get_target_property(HE_TEST_PLUGIN_IDS HEngineTests HE_STATIC_PLUGIN_IDS)
list(FIND HE_TEST_PLUGIN_IDS
    "he.offline-reference-renderer" HE_TEST_OFFLINE_INDEX)
if(HE_TEST_OFFLINE_INDEX EQUAL -1)
    target_compile_definitions(HEngineTests PRIVATE
        HE_WITH_OFFLINE_REFERENCE_RENDERER_TESTS=0)
else()
    target_compile_definitions(HEngineTests PRIVATE
        HE_WITH_OFFLINE_REFERENCE_RENDERER_TESTS=1)
endif()

set_target_properties(HEngineTests PROPERTIES
    FOLDER "Engine/Tests"
    RUNTIME_OUTPUT_DIRECTORY "${BINARY_ROOT_DIR}"
)
