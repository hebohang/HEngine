include_guard(GLOBAL)

function(he_resolve_vendor_crt)
    if(NOT MSVC)
        message(FATAL_ERROR "HEngine requires the supported MSVC toolchain")
    endif()
    set(HE_VC_CRT_DIRECTORY "Microsoft.VC${MSVC_TOOLSET_VERSION}.CRT")
    set(HE_VC_REDIST_X64_DIR "" CACHE PATH
        "Optional fixed Visual Studio x64 CRT redistributable directory.")
    if(NOT HE_VC_REDIST_X64_DIR)
        if(DEFINED ENV{VCToolsRedistDir} AND NOT "$ENV{VCToolsRedistDir}" STREQUAL "")
            file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}" HE_VC_REDIST_ROOT)
            set(HE_VC_REDIST_X64_DIR
                "${HE_VC_REDIST_ROOT}/${HE_WINDOWS_ARCH}/${HE_VC_CRT_DIRECTORY}")
        elseif(CMAKE_GENERATOR_INSTANCE)
            get_filename_component(HE_MSVC_COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
            get_filename_component(HE_MSVC_COMPILER_HOST "${HE_MSVC_COMPILER_BIN}" DIRECTORY)
            get_filename_component(HE_MSVC_COMPILER_BIN_ROOT "${HE_MSVC_COMPILER_HOST}" DIRECTORY)
            get_filename_component(HE_MSVC_TOOLSET_ROOT "${HE_MSVC_COMPILER_BIN_ROOT}" DIRECTORY)
            get_filename_component(HE_MSVC_TOOLSET_VERSION "${HE_MSVC_TOOLSET_ROOT}" NAME)
            string(REGEX MATCH "^[0-9]+[.][0-9]+" HE_MSVC_TOOLSET_FAMILY
                "${HE_MSVC_TOOLSET_VERSION}")
            file(GLOB HE_VC_REDIST_VERSION_DIRS LIST_DIRECTORIES TRUE
                "${CMAKE_GENERATOR_INSTANCE}/VC/Redist/MSVC/${HE_MSVC_TOOLSET_FAMILY}.*")
            list(SORT HE_VC_REDIST_VERSION_DIRS COMPARE NATURAL ORDER DESCENDING)
            foreach(HE_VC_REDIST_VERSION_DIR IN LISTS HE_VC_REDIST_VERSION_DIRS)
                set(HE_VC_REDIST_CANDIDATE
                    "${HE_VC_REDIST_VERSION_DIR}/${HE_WINDOWS_ARCH}/${HE_VC_CRT_DIRECTORY}")
                if(EXISTS "${HE_VC_REDIST_CANDIDATE}/vcruntime140.dll")
                    set(HE_VC_REDIST_X64_DIR "${HE_VC_REDIST_CANDIDATE}")
                    break()
                endif()
            endforeach()
        endif()
    endif()
    if(NOT HE_VC_REDIST_X64_DIR)
        message(FATAL_ERROR
            "Could not locate the Visual Studio redistributable matching "
            "${CMAKE_CXX_COMPILER}. Set HE_VC_REDIST_X64_DIR to the licensed "
            "x64/${HE_VC_CRT_DIRECTORY} directory.")
    endif()
    get_filename_component(HE_VC_REDIST_X64_DIR "${HE_VC_REDIST_X64_DIR}" ABSOLUTE)
    set(HE_VC_REDIST_X64_DIR "${HE_VC_REDIST_X64_DIR}" CACHE PATH
        "Fixed Visual Studio x64 CRT redistributable directory." FORCE)
    set(HE_PACKAGE_VCRUNTIME "${HE_VC_REDIST_X64_DIR}/vcruntime140.dll")
    if(NOT EXISTS "${HE_PACKAGE_VCRUNTIME}" OR IS_DIRECTORY "${HE_PACKAGE_VCRUNTIME}")
        message(FATAL_ERROR
            "The official Visual Studio x64 redistributable is missing vcruntime140.dll: "
            "${HE_PACKAGE_VCRUNTIME}")
    endif()
    file(TO_CMAKE_PATH "$ENV{SystemRoot}/System32" HE_WINDOWS_SYSTEM32_DIR)
    string(TOLOWER "${HE_PACKAGE_VCRUNTIME}" HE_PACKAGE_VCRUNTIME_KEY)
    string(TOLOWER "${HE_WINDOWS_SYSTEM32_DIR}/" HE_WINDOWS_SYSTEM32_KEY)
    string(FIND "${HE_PACKAGE_VCRUNTIME_KEY}" "${HE_WINDOWS_SYSTEM32_KEY}" HE_SYSTEM32_RUNTIME_POSITION)
    if(HE_SYSTEM32_RUNTIME_POSITION EQUAL 0)
        message(FATAL_ERROR "HEngine must never source VC runtime files from System32")
    endif()
    message(STATUS "Vendor VC runtime source: ${HE_PACKAGE_VCRUNTIME}")
endfunction()

function(he_deploy_vendor_crt targetName)
    if(NOT WIN32 OR TARGET ${targetName}VendorCrt)
        return()
    endif()
    he_resolve_vendor_crt()
    set(runtimeFiles "")
    foreach(name msvcp140.dll vcruntime140.dll vcruntime140_1.dll)
        if(HE_WINDOWS_ARCH STREQUAL "arm64" AND name STREQUAL "vcruntime140_1.dll" AND
           NOT EXISTS "${HE_VC_REDIST_X64_DIR}/${name}")
            continue()
        endif()
        if(NOT EXISTS "${HE_VC_REDIST_X64_DIR}/${name}")
            message(FATAL_ERROR "Missing licensed ${HE_WINDOWS_ARCH} VC runtime: ${name}")
        endif()
        list(APPEND runtimeFiles "${HE_VC_REDIST_X64_DIR}/${name}")
    endforeach()
    add_custom_target(${targetName}VendorCrt
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${targetName}>/Licenses"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${runtimeFiles} "$<TARGET_FILE_DIR:${targetName}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Engine/Resources/Licenses/Microsoft-Visual-Cpp-Runtime.txt"
            "$<TARGET_FILE_DIR:${targetName}>/Licenses/Microsoft-Visual-Cpp-Runtime.txt"
        COMMAND_EXPAND_LISTS VERBATIM)
    add_dependencies(${targetName} ${targetName}VendorCrt)
endfunction()
