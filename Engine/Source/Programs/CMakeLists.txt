set(ProgramsFolder "Programs")

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_FOLDER ${ProgramsFolder})
set(ProgramsOutDir "${BINARY_ROOT_DIR}/Programs")

function(ADD_PROGRAM TARGET_NAME)
    if(NOT TARGET ${TARGET_NAME})
        add_executable(${TARGET_NAME})
        set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "Programs/${TARGET_NAME}") 
        if(PACK_GAME AND "${TARGET_NAME}" STREQUAL "HCooker")
            set_target_properties(${TARGET_NAME} PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/Programs/${TARGET_NAME}/$<CONFIG>"
            )
        else()
            set_target_properties(${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY         "${ProgramsOutDir}/${TARGET_NAME}")
            set_target_properties(${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${ProgramsOutDir}/${TARGET_NAME}")
            set_target_properties(${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${ProgramsOutDir}/${TARGET_NAME}")
        endif()

        if (WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
            target_link_directories(${TARGET_NAME} PRIVATE
                "$ENV{VCToolsInstallDir}/ATLMFC/lib/x64"
                "$ENV{VCToolsInstallDir}/lib/x64"
                "$ENV{UniversalCRTSdkDir}/Lib/$ENV{UCRTVersion}/ucrt/x64"
                "$ENV{WindowsSdkDir}/Lib/$ENV{WindowsSDKVersion}/um/x64"
            )
        endif()

        set(TARGET_NAME ${TARGET_NAME} PARENT_SCOPE)
    endif()
endfunction()

add_subdirectory(HEngineParser)
add_subdirectory(HShaderCompiler)
if(NOT PACK_GAME)
    add_subdirectory(HAssetImporter)
endif()
add_subdirectory(HCooker)
if(NOT PACK_GAME)
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
