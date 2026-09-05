cmake_minimum_required(VERSION 3.20)

if(DEFINED HE_PLUGIN_ACTIVE_HOSTS_CSV)
    string(REPLACE "," ";" HE_PLUGIN_ACTIVE_HOSTS "${HE_PLUGIN_ACTIVE_HOSTS_CSV}")
endif()

foreach(requiredVariable IN ITEMS
    HE_PLUGIN_MODULE
    HE_PLUGIN_ROOT
    HE_PLUGIN_CONFIG
    HE_PLUGIN_GENERATED_DIR
    HE_PLUGIN_ACTIVE_HOSTS)
    if(NOT DEFINED ${requiredVariable} OR "${${requiredVariable}}" STREQUAL "")
        message(FATAL_ERROR "Probe requires -D${requiredVariable}=...")
    endif()
endforeach()

include("${HE_PLUGIN_MODULE}")
if(DEFINED HE_PLUGIN_EXPECT_IDS_CSV)
    string(REPLACE "," ";" HE_PLUGIN_EXPECT_IDS "${HE_PLUGIN_EXPECT_IDS_CSV}")
endif()
if(DEFINED HE_PLUGIN_EXPECT_ENABLED_IDS_CSV)
    string(REPLACE "," ";" HE_PLUGIN_EXPECT_ENABLED_IDS
        "${HE_PLUGIN_EXPECT_ENABLED_IDS_CSV}")
endif()
if(DEFINED HE_PLUGIN_EXPECT_ANCHOR_TEXT_CSV)
    string(REPLACE "," ";" HE_PLUGIN_EXPECT_ANCHOR_TEXT
        "${HE_PLUGIN_EXPECT_ANCHOR_TEXT_CSV}")
endif()
set(discoveryArguments
    ROOT "${HE_PLUGIN_ROOT}"
    CONFIG "${HE_PLUGIN_CONFIG}"
    GENERATED_DIR "${HE_PLUGIN_GENERATED_DIR}"
    ACTIVE_HOSTS ${HE_PLUGIN_ACTIVE_HOSTS}
    CONFIGURATION Debug
    PLATFORM Windows)
if(NOT HE_PLUGIN_BUILD_DISTRIBUTION)
    set(HE_PLUGIN_BUILD_DISTRIBUTION Development)
endif()
list(APPEND discoveryArguments
    BUILD_DISTRIBUTION "${HE_PLUGIN_BUILD_DISTRIBUTION}")
he_discover_static_plugins(${discoveryArguments})

if(DEFINED HE_PLUGIN_EXPECT_IDS)
    if(NOT HE_STATIC_PLUGIN_IDS STREQUAL HE_PLUGIN_EXPECT_IDS)
        message(FATAL_ERROR
            "Expected plugin order '${HE_PLUGIN_EXPECT_IDS}', got '${HE_STATIC_PLUGIN_IDS}'")
    endif()
endif()

if(DEFINED HE_PLUGIN_EXPECT_ENABLED_IDS)
    if(NOT HE_ENABLED_STATIC_PLUGIN_IDS STREQUAL HE_PLUGIN_EXPECT_ENABLED_IDS)
        message(FATAL_ERROR
            "Expected enabled plugins '${HE_PLUGIN_EXPECT_ENABLED_IDS}', got '${HE_ENABLED_STATIC_PLUGIN_IDS}'")
    endif()
endif()

if(DEFINED HE_PLUGIN_INSPECT_HOST)
    set(anchorVariable "HE_STATIC_PLUGIN_ANCHOR_${HE_PLUGIN_INSPECT_HOST}")
    if(NOT EXISTS "${${anchorVariable}}")
        message(FATAL_ERROR "Expected generated anchor '${${anchorVariable}}'")
    endif()
    file(READ "${${anchorVariable}}" anchorContent)
    foreach(expectedText IN LISTS HE_PLUGIN_EXPECT_ANCHOR_TEXT)
        string(FIND "${anchorContent}" "${expectedText}" expectedPosition)
        if(expectedPosition EQUAL -1)
            message(FATAL_ERROR
                "Generated ${HE_PLUGIN_INSPECT_HOST} anchor lacks '${expectedText}'")
        endif()
    endforeach()
endif()

message(STATUS
    "HEngine plugin probe PASS: ids=${HE_STATIC_PLUGIN_IDS}; enabled=${HE_ENABLED_STATIC_PLUGIN_IDS}")
