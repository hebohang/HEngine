if(NOT DEFINED BUILD_CONFIG OR NOT BUILD_CONFIG STREQUAL "Release")
    message(FATAL_ERROR
        "PackageGame only supports the Release configuration. "
        "Use: cmake --build <build-dir> --config Release --target PackageGame")
endif()

if(NOT PACK_GAME)
    message(FATAL_ERROR "PackageGame requires PACK_GAME=ON")
endif()
if(HE_BUILD_DEV_GAME)
    message(FATAL_ERROR "PackageGame requires HE_BUILD_DEV_GAME=OFF")
endif()
if(HE_ENABLE_GM)
    message(FATAL_ERROR "PackageGame requires HE_ENABLE_GM=OFF")
endif()

message(STATUS
    "Verified packaged-game configuration: ${BUILD_CONFIG}, "
    "PACK_GAME=ON, HE_BUILD_DEV_GAME=OFF, HE_ENABLE_GM=OFF")
