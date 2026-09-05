if(NOT COMMAND he_add_discovered_static_plugins)
    message(FATAL_ERROR
        "Engine plugins must be discovered through CMake/HEnginePlugins.cmake")
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_FOLDER "Plugins")

# Discovery has already validated every manifest and the project host-enable
# file. Only plugins enabled for at least one active host enter this build graph.
he_add_discovered_static_plugins()

get_property(PluginsHeaderFiles GLOBAL PROPERTY HE_STATIC_PLUGIN_HEADER_FILES)
get_property(PluginsSourceFiles GLOBAL PROPERTY HE_STATIC_PLUGIN_SOURCE_FILES)
set(HEnginePluginsHeaders "${PluginsHeaderFiles}" PARENT_SCOPE)
set(HEnginePluginsSources "${PluginsSourceFiles}" PARENT_SCOPE)
