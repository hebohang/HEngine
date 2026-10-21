set_target_properties(HGame PROPERTIES OUTPUT_NAME hegame
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_sources(HGame PRIVATE "${EngineRuntimeDir}/Platform/Android/AndroidHost.cpp")
target_link_libraries(HGame PRIVATE android log dl)
target_link_options(HGame PRIVATE "-Wl,--no-undefined" "-Wl,-z,max-page-size=16384")
