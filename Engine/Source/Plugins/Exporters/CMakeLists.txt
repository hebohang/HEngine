ADD_PLUGIN(Exporters)

add_subdirectory(MeshExporter)

PLUGIN_ADD_SOURCE(
    ExportersPluginEntry.cpp
)

target_link_libraries(Exporters
    PRIVATE spdlog
)

END_PLUGIN()
