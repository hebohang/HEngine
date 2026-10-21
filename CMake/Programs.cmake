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
                "$ENV{VCToolsInstallDir}/ATLMFC/lib/${HE_WINDOWS_ARCH}"
                "$ENV{VCToolsInstallDir}/lib/${HE_WINDOWS_ARCH}"
                "$ENV{UniversalCRTSdkDir}/Lib/$ENV{UCRTVersion}/ucrt/${HE_WINDOWS_ARCH}"
                "$ENV{WindowsSdkDir}/Lib/$ENV{WindowsSDKVersion}/um/${HE_WINDOWS_ARCH}"
            )
        endif()

        set(TARGET_NAME ${TARGET_NAME} PARENT_SCOPE)
    endif()
endfunction()
