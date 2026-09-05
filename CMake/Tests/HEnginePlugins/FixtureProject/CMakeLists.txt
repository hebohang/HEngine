cmake_minimum_required(VERSION 3.20)
project(HEnginePluginManifestFixture LANGUAGES NONE)

foreach(requiredVariable IN ITEMS
    HE_PLUGIN_MODULE
    HE_PLUGIN_ROOT
    HE_PLUGIN_CONFIG
    HE_PLUGIN_GENERATED_DIR
    HE_PLUGIN_ACTIVE_HOSTS)
    if(NOT DEFINED ${requiredVariable} OR "${${requiredVariable}}" STREQUAL "")
        message(FATAL_ERROR "FixtureProject requires -D${requiredVariable}=...")
    endif()
endforeach()

include("${HE_PLUGIN_MODULE}")
he_discover_static_plugins(
    ROOT "${HE_PLUGIN_ROOT}"
    CONFIG "${HE_PLUGIN_CONFIG}"
    GENERATED_DIR "${HE_PLUGIN_GENERATED_DIR}"
    ACTIVE_HOSTS ${HE_PLUGIN_ACTIVE_HOSTS}
    PLATFORM Windows
    CONFIGURATION Debug
    BUILD_DISTRIBUTION Development)
he_add_discovered_static_plugins()

foreach(expectedTarget IN LISTS HE_PLUGIN_EXPECT_TARGETS)
    if(NOT TARGET "${expectedTarget}")
        message(FATAL_ERROR "Expected enabled plugin target '${expectedTarget}'")
    endif()
endforeach()
foreach(disabledTarget IN LISTS HE_PLUGIN_EXPECT_NO_TARGETS)
    if(TARGET "${disabledTarget}")
        message(FATAL_ERROR "Disabled plugin created target '${disabledTarget}'")
    endif()
endforeach()

message(STATUS
    "HEngine plugin target fixture PASS: enabled=${HE_ENABLED_STATIC_PLUGIN_MODULES}")
