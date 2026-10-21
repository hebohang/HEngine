ADD_PROGRAM(HAssetImporter)

set(src
    "${CMAKE_CURRENT_SOURCE_DIR}/Main.cpp"
)

target_sources(${TARGET_NAME} PRIVATE ${src})

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${src})

target_compile_features(${TARGET_NAME} PRIVATE cxx_std_20)

target_compile_definitions(${TARGET_NAME} PRIVATE
    $<$<CONFIG:Debug>:HE_ASSET_IMPORTER_PLUGIN_CONFIG_DEBUG=1>
    $<$<CONFIG:RelWithDebInfo>:HE_ASSET_IMPORTER_PLUGIN_CONFIG_RELWITHDEBINFO=1>
    $<$<CONFIG:Release>:HE_ASSET_IMPORTER_PLUGIN_CONFIG_RELEASE=1>
    $<$<CONFIG:MinSizeRel>:HE_ASSET_IMPORTER_PLUGIN_CONFIG_MINSIZEREL=1>
)

target_include_directories(${TARGET_NAME} PRIVATE
    "${EngineSourceDir}"
    "${EngineRuntimeDir}"
    "${HE_GENERATED_SOURCE_DIR}"
)

target_link_libraries(${TARGET_NAME} PRIVATE
    HEngineRuntime
)

he_attach_static_plugins(HAssetImporter Tools)

# Editor-enabled runtime links Embree; this program lives below bin/Programs,
# so it cannot use DLLs staged next to the editor executable.
if(WIN32 AND NOT HE_WINDOWS_ARCH STREQUAL "arm64" AND HE_WITH_EDITOR AND TARGET embree)
    foreach(runtimeDll embree4.dll tbb12.dll tbbmalloc.dll)
        add_custom_command(TARGET HAssetImporter POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${ThirdPartyDir}/embree/dll/x64/${runtimeDll}"
                "$<TARGET_FILE_DIR:HAssetImporter>/${runtimeDll}"
            VERBATIM)
    endforeach()
endif()
