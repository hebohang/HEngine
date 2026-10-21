ADD_PLUGIN(Importers)

add_subdirectory(MeshImporter)

PLUGIN_ADD_SOURCE(
    ImportersPluginEntry.cpp
)

target_link_libraries(Importers
    PRIVATE glm
    PRIVATE spdlog
    PRIVATE nlohmann_json::nlohmann_json
    PRIVATE assimp
    PRIVATE HEngineRuntime
)

END_PLUGIN()
if(HE_ENABLE_PHYSICS_3D)
    target_sources(Importers PRIVATE "${EngineRuntimeDir}/Platform/PhysicsMeshCooker.cpp")
endif()
target_include_directories(Importers PRIVATE "${EngineRuntimeDir}")
