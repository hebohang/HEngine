ADD_PLUGIN(RenderDoc)

add_subdirectory(RenderDoc)

PLUGIN_ADD_SOURCE(
    RenderDocPluginEntry.cpp
)

target_link_libraries(RenderDoc
    PRIVATE spdlog
)

END_PLUGIN()
