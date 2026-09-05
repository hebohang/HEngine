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
)

END_PLUGIN()
