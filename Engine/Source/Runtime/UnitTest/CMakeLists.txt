add_executable(HEngineTests
    ScriptCoreTests.cpp
    ScriptEcsTests.cpp
    TestMain.cpp
    BuildFeatureTests.cpp
    AgentEventLogTests.cpp
    FileMailboxAgentTransportTests.cpp
    "${EngineSourceDir}/Game/AgentRuntime/FileMailboxAgentTransport.cpp"
    "${EngineSourceDir}/Game/AgentRuntime/AgentCommandRegistry.cpp"
    "${EngineSourceDir}/Game/AgentRuntime/AgentEventLog.cpp"
    AntiAliasingTests.cpp
    AnimationPoseTests.cpp
    SequenceTests.cpp
    PrefabSubtreeTests.cpp
    AudioLifecycleTests.cpp
    AsyncLoggingTests.cpp
    StatisticsBufferTests.cpp
    DelegateBufferTests.cpp
    DirectionalShadowTests.cpp
    TaskSchedulerTests.cpp
    TextureUploadStateTests.cpp
    BindlessDescriptorManagerTests.cpp
    ConfigManagerTests.cpp
    DevelopmentLaunchPathsTests.cpp
    ProjectIdentityTests.cpp
    GameDisplaySettingsTests.cpp
    CornellSceneSerializationTests.cpp
    DevCommandTests.cpp
    EffectTransformBatchTests.cpp
    EffekseerRegressionManifestTests.cpp
    EffekseerRuntimeLifecycleTests.cpp
    GamepadInputTests.cpp
    InputActionsTests.cpp
    NavigationGeometryTests.cpp
    PhysicsScriptContactTests.cpp
    HardwareRayTracingTests.cpp
    MaterialHotReloadTests.cpp
    MaterialParameterPackingTests.cpp
    MaterialPipelineKeyTests.cpp
	MaterialResourceCacheTests.cpp
	MaterialThumbnailCacheKeyTests.cpp
    MaterialSerializationTests.cpp
    MaterialSamplingTests.cpp
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
    SceneEditingTests.cpp
    SystemLifecycleTests.cpp
    SurvivorEnemyBehaviorTests.cpp
    SurvivorPickupBatchTests.cpp
    SurvivorProjectContractTests.cpp
    SurvivorSpatialIndexTests.cpp
    SurvivorTargetingBatchTests.cpp
)

target_include_directories(HEngineTests PRIVATE
    "${EngineSourceDir}"
    "${EngineRuntimeDir}"
)
target_link_libraries(HEngineTests PRIVATE HEngineRuntime)
he_deploy_fsr31(HEngineTests)
he_attach_static_plugins(HEngineTests Tests)
get_target_property(HE_TEST_PLUGIN_IDS HEngineTests HE_STATIC_PLUGIN_IDS)
if("he.script.csharp" IN_LIST HE_TEST_PLUGIN_IDS)
    target_sources(HEngineTests PRIVATE "${EngineSourceDir}/Plugins/CSharpScript/Tests/CSharpRuntimeTests.cpp")
    target_compile_definitions(HEngineTests PRIVATE HE_WITH_CSHARP_TESTS=1)
else()
    target_compile_definitions(HEngineTests PRIVATE HE_WITH_CSHARP_TESTS=0)
endif()
if("he.script.python" IN_LIST HE_TEST_PLUGIN_IDS)
    target_sources(HEngineTests PRIVATE "${EngineSourceDir}/Plugins/PythonScript/Tests/PythonRuntimeTests.cpp")
    target_compile_definitions(HEngineTests PRIVATE HE_WITH_PYTHON_TESTS=1)
    target_include_directories(HEngineTests PRIVATE
        "${ThirdPartyDir}/Python/include" "${ThirdPartyDir}/pybind11/include")
else()
    target_compile_definitions(HEngineTests PRIVATE HE_WITH_PYTHON_TESTS=0)
endif()
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
