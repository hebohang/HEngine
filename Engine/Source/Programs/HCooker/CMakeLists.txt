ADD_PROGRAM(HCooker)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../../CMake/ScriptCore.cmake")


set(src
    "${CMAKE_CURRENT_SOURCE_DIR}/Cooker.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Cooker.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Main.cpp"
    "${EngineRuntimeDir}/Graphics/Material/MaterialShaderManifest.h"
    "${EngineRuntimeDir}/Graphics/Material/MaterialShaderManifest.cpp"
    "${EngineRuntimeDir}/Resource/RuntimeResourceCatalog/RuntimeResourceWarmupSchema.h"
    "${EngineRuntimeDir}/Resource/RuntimeResourceCatalog/RuntimeResourceWarmupSchema.cpp"
    "${EngineRuntimeDir}/Utils/JsonUtils/CookedJsonFormat.h"
)

target_sources(${TARGET_NAME} PRIVATE ${src})

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/Cooker.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Cooker.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Main.cpp"
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
target_compile_definitions(${TARGET_NAME} PRIVATE HE_ENABLE_PHYSICS_3D=$<BOOL:${HE_ENABLE_PHYSICS_3D}>)
if(HE_ENABLE_PHYSICS_3D)
    target_sources(${TARGET_NAME} PRIVATE "${EngineRuntimeDir}/Platform/PhysicsMeshCooker.cpp")
    target_link_libraries(${TARGET_NAME} PRIVATE Jolt)
endif()

target_include_directories(${TARGET_NAME} PRIVATE
    "${EngineSourceDir}"
    "${ThirdPartyDir}/Effekseer/src"
)

target_link_libraries(${TARGET_NAME} PRIVATE
    HEngineScriptCore
    HEArchive
    nlohmann_json::nlohmann_json
)

target_compile_definitions(HCooker PRIVATE HE_RENDER_DX12=$<BOOL:${HE_RENDER_DX12}> HE_RENDER_VULKAN=$<BOOL:${HE_RENDER_VULKAN}>)
if(HE_RENDER_DX12)
    target_link_libraries(HCooker PRIVATE EffekseerRendererDX12)
endif()
if(HE_RENDER_VULKAN)
    target_link_libraries(HCooker PRIVATE EffekseerRendererVulkan)
endif()
