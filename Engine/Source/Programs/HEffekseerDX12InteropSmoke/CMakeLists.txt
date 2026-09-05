ADD_PROGRAM(HEffekseerDX12InteropSmoke)

target_sources(${TARGET_NAME} PRIVATE
    Main.cpp
)

target_include_directories(${TARGET_NAME} PRIVATE
    "${EngineSourceDir}"
    "${EngineRuntimeDir}"
)

target_link_libraries(${TARGET_NAME} PRIVATE
    HEngineRuntime
    EffekseerRendererDX12
    nvrhi
)

set_target_properties(${TARGET_NAME} PROPERTIES CXX_STANDARD 20)

if(NOT TARGET HEngineRuntimeShaders)
    message(FATAL_ERROR
        "HEffekseerDX12InteropSmoke requires HEngineRuntimeShaders")
endif()
add_dependencies(${TARGET_NAME} HEngineRuntimeShaders)
