set(ProgramsFolder "Programs")

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_FOLDER ${ProgramsFolder})
set(ProgramsOutDir "${BINARY_ROOT_DIR}/Programs")

include("${CMAKE_SOURCE_DIR}/CMake/Programs.cmake")

if(NOT HE_CODEGEN_PARSER_EXECUTABLE)
    add_subdirectory(HEngineParser)
endif()
add_subdirectory(HShaderCompiler)
if(HE_BUILD_RENDER_SMOKES AND NOT PACK_GAME)
    add_subdirectory(HEngineFsrSmoke)
endif()
if(HE_BUILD_ASSET_TOOLS AND NOT PACK_GAME)
    add_subdirectory(HAssetImporter)
endif()
if(HE_BUILD_ASSET_TOOLS OR PACK_GAME)
    add_subdirectory(HCooker)
endif()
if(HE_BUILD_RENDER_SMOKES AND NOT PACK_GAME AND HE_RENDER_DX12)
    add_subdirectory(HEngineDX12Smoke)
    add_subdirectory(HEngineDX12SceneSmoke)
    add_subdirectory(HEffekseerDX12InteropSmoke)
    add_subdirectory(HEngineImGuiDX12Smoke)
    add_subdirectory(HEngineRmlDX12Smoke)

    if(WIN32)
        foreach(dx12SmokeTarget IN ITEMS
            HEngineDX12Smoke
            HEngineDX12SceneSmoke
            HEffekseerDX12InteropSmoke
            HEngineImGuiDX12Smoke
            HEngineRmlDX12Smoke)
            he_enable_d3d12_agility(${dx12SmokeTarget})
        endforeach()
    endif()
endif()

set(ProgramsOutDir ${ProgramsOutDir} PARENT_SCOPE)
