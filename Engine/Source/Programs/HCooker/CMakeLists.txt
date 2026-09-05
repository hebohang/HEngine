ADD_PROGRAM(HCooker)

set(ZipSourceDir "${ThirdPartyDir}/assimp/contrib/zip/src")
set(src
    "${CMAKE_CURRENT_SOURCE_DIR}/Cooker.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Cooker.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Main.cpp"
    "${EngineRuntimeDir}/Graphics/Material/MaterialShaderManifest.h"
    "${EngineRuntimeDir}/Graphics/Material/MaterialShaderManifest.cpp"
    "${EngineRuntimeDir}/Resource/RuntimeResourceCatalog/RuntimeResourceWarmupSchema.h"
    "${EngineRuntimeDir}/Resource/RuntimeResourceCatalog/RuntimeResourceWarmupSchema.cpp"
    "${EngineRuntimeDir}/Utils/JsonUtils/CookedJsonFormat.h"
    "${ZipSourceDir}/miniz.h"
    "${ZipSourceDir}/zip.c"
    "${ZipSourceDir}/zip.h"
)

target_sources(${TARGET_NAME} PRIVATE ${src})

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/Cooker.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Cooker.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Main.cpp"
)
source_group("ThirdParty/zip" FILES
    "${ZipSourceDir}/miniz.h"
    "${ZipSourceDir}/zip.c"
    "${ZipSourceDir}/zip.h"
)
source_group("Runtime/Resource" FILES
    "${EngineRuntimeDir}/Resource/RuntimeResourceCatalog/RuntimeResourceWarmupSchema.h"
    "${EngineRuntimeDir}/Resource/RuntimeResourceCatalog/RuntimeResourceWarmupSchema.cpp"
)
source_group("Runtime/Graphics/Material" FILES
    "${EngineRuntimeDir}/Graphics/Material/MaterialShaderManifest.h"
    "${EngineRuntimeDir}/Graphics/Material/MaterialShaderManifest.cpp"
)

target_compile_features(${TARGET_NAME} PRIVATE cxx_std_20)
target_compile_definitions(${TARGET_NAME} PRIVATE MINIZ_NO_TIME)

target_include_directories(${TARGET_NAME} PRIVATE
    "${EngineSourceDir}"
    "${ThirdPartyDir}/Effekseer/src"
    "${ZipSourceDir}"
)

target_link_libraries(${TARGET_NAME} PRIVATE
    EffekseerRendererDX12
    EffekseerRendererVulkan
    nlohmann_json::nlohmann_json
)
