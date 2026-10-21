add_subdirectory(AssetManager)
add_subdirectory(ConfigManager)
add_subdirectory(ModeManager)
add_subdirectory(ResType)
add_subdirectory(RuntimeResourceCatalog)
add_subdirectory(RuntimeResourceRegistry)

RUNTIME_ADD_SOURCE(
    AssetReadAhead.h AssetReadAhead.cpp
    RuntimeProjectResourceWarmup.h RuntimeProjectResourceWarmup.cpp
)
