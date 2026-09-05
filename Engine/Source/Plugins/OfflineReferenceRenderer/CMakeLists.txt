ADD_PLUGIN(OfflineReferenceRenderer)

PLUGIN_ADD_SOURCE(
    OfflineReferenceRendererPluginEntry.cpp
    Source/OfflineReferenceRendererSession.h
    Source/OfflineReferenceRendererSession.cpp
    Source/OfflineReferenceRendererSettingsJson.h
    Source/OfflineReferenceRendererSettingsJson.cpp
    Tests/OfflineReferenceRendererSessionTests.h
)

set_property(TARGET OfflineReferenceRenderer PROPERTY HE_PLUGIN_SHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/HardwareRayTracingReference.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/OfflineReferencePreview.hlsl"
)

target_include_directories(OfflineReferenceRenderer PRIVATE
    "${EngineRuntimeDir}"
)

target_link_libraries(OfflineReferenceRenderer PRIVATE
    glm
    nvrhi
    nlohmann_json::nlohmann_json
    spdlog
)

END_PLUGIN()
