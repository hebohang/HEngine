ADD_PROGRAM(HEngineDX12Smoke)

target_sources(${TARGET_NAME} PRIVATE
    Main.cpp
)

target_include_directories(${TARGET_NAME} PRIVATE
    "${EngineSourceDir}"
    "${EngineRuntimeDir}"
)

target_link_libraries(${TARGET_NAME} PRIVATE HEngineRuntime nvrhi)

set_target_properties(${TARGET_NAME} PROPERTIES CXX_STANDARD 20)

set(HE_DX12_SMOKE_SHADER_SOURCE
    "${EngineContentShadersDir}/Validation/DX12Triangle.hlsl")
set(HE_DX12_SMOKE_SHADER_CACHE_DIR
    "${EngineGeneratedShadersDir}/CACHES/DXIL")
set(HE_DX12_SMOKE_SHADER_OUTPUTS
    "${HE_DX12_SMOKE_SHADER_CACHE_DIR}/DX12Triangle.vs"
    "${HE_DX12_SMOKE_SHADER_CACHE_DIR}/DX12Triangle.ps"
    "${HE_DX12_SMOKE_SHADER_CACHE_DIR}/DX12Triangle.vs.cachekey"
    "${HE_DX12_SMOKE_SHADER_CACHE_DIR}/DX12Triangle.ps.cachekey"
    "${EngineGeneratedShadersDir}/CACHES/DX12Triangle.meta")

add_custom_command(
    OUTPUT ${HE_DX12_SMOKE_SHADER_OUTPUTS}
    COMMAND $<TARGET_FILE:HShaderCompiler>
        --rootPath "${EngineContentShadersDir}"
        --outputPath "${EngineGeneratedShadersDir}"
        --shader "${HE_DX12_SMOKE_SHADER_SOURCE}"
    DEPENDS
        HShaderCompiler
        "${HE_DX12_SMOKE_SHADER_SOURCE}"
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:HShaderCompiler>"
    COMMENT "Compiling the DX12 indexed-triangle smoke shader with DXC"
    VERBATIM)

add_custom_target(HEngineDX12SmokeShaders
    DEPENDS ${HE_DX12_SMOKE_SHADER_OUTPUTS})
add_dependencies(${TARGET_NAME} HEngineDX12SmokeShaders)
