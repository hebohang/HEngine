include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/WindowsVendorRuntime.cmake")

option(HE_FSR31 "Enable the official FSR 3.1 DX12/Vulkan upscaler runtimes" ON)
if(NOT WIN32)
    set(HE_FSR31 OFF)
endif()
set(HE_FSR31_DX12 OFF)
set(HE_FSR31_VULKAN OFF)
set(HE_FSR31_SOURCE_SHA256 "")
set(HE_FSR31_RUNTIME_SHA256 "")

if(HE_FSR31)
    set(HE_FSR31_DX12 ${HE_RENDER_DX12})
    set(HE_FSR31_VULKAN ${HE_RENDER_VULKAN})
    if(HE_WINDOWS_ARCH STREQUAL "arm64")
        # Vulkan rendering remains available; only the optional FSR backend is absent.
        set(HE_FSR31_VULKAN OFF)
        set(HE_FSR31_RUNTIME_DIR "${HE_FSR31_ROOT}/ffx-api/bin")
        foreach(fsrRequired IN ITEMS ffx-api/include/ffx_api/ffx_api.h sdk/LICENSE.txt)
            if(NOT EXISTS "${HE_FSR31_ROOT}/${fsrRequired}")
                message(FATAL_ERROR "Incomplete ARM64 FidelityFX source SDK: ${fsrRequired}")
            endif()
        endforeach()
        file(READ "${HE_WINDOWS_ARM64_ROOT}/fsr-build/HEngine.FsrBuild.json" fsrReceipt)
        file(READ "${CMAKE_SOURCE_DIR}/Tools/Dependencies/native-runtime.arm64.lock.json" fsrLock)
        string(JSON fsrArchitecture GET "${fsrReceipt}" architecture)
        string(JSON fsrBackend GET "${fsrReceipt}" backend)
        string(JSON HE_FSR31_SOURCE_SHA256 GET "${fsrReceipt}" sourceArchiveSha256)
        string(JSON fsrSourcePin GET "${fsrLock}" fsrSourceArchiveSha256)
        string(JSON HE_FSR31_RUNTIME_SHA256 GET "${fsrReceipt}" runtimeSha256)
        file(SHA256 "${HE_FSR31_RUNTIME_DIR}/amd_fidelityfx_dx12.dll" fsrActualHash)
        if(NOT fsrArchitecture STREQUAL "arm64" OR NOT fsrBackend STREQUAL "dx12" OR
           NOT HE_FSR31_SOURCE_SHA256 STREQUAL fsrSourcePin OR
           NOT HE_FSR31_RUNTIME_SHA256 STREQUAL fsrActualHash)
            message(FATAL_ERROR "ARM64 FSR source/runtime receipt mismatch; prepare native dependencies again.")
        endif()
        he_require_windows_binary_architecture("${HE_FSR31_RUNTIME_DIR}/amd_fidelityfx_dx12.dll")
    else()
    set(HE_FSR31_ROOT "${CMAKE_SOURCE_DIR}/out/agent/dependencies/fsr-3.1/sdk-1.1.4" CACHE PATH
        "Extracted FidelityFX SDK 1.1.4 (FSR 3.1.4), including signed runtimes")
    set(fsrArchive "${CMAKE_SOURCE_DIR}/out/agent/dependencies/fsr-3.1/FidelityFX-SDK-v1.1.4.zip")
    set(fsrHash "0216556bfb0e243cec30004a2a98d38f4e3f7406cb7938e3c1b85c758e95d952")
    if(NOT EXISTS "${HE_FSR31_ROOT}/.he-verified-${fsrHash}")
        if(NOT EXISTS "${fsrArchive}")
            get_filename_component(fsrCache "${fsrArchive}" DIRECTORY)
            file(MAKE_DIRECTORY "${fsrCache}")
            file(DOWNLOAD
                "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v1.1.4/FidelityFX-SDK-v1.1.4.zip"
                "${fsrArchive}.download" EXPECTED_HASH "SHA256=${fsrHash}"
                TLS_VERIFY ON SHOW_PROGRESS STATUS fsrDownloadStatus)
            list(GET fsrDownloadStatus 0 fsrDownloadCode)
            if(NOT fsrDownloadCode EQUAL 0)
                message(FATAL_ERROR "FidelityFX SDK download failed: ${fsrDownloadStatus}")
            endif()
            file(RENAME "${fsrArchive}.download" "${fsrArchive}")
        endif()
        file(SHA256 "${fsrArchive}" fsrActualHash)
        if(NOT fsrActualHash STREQUAL fsrHash)
            message(FATAL_ERROR "FidelityFX SDK checksum mismatch: ${fsrArchive}")
        endif()
        # Headers and signed x64 runtime only; no sample assets or SDK build.
        file(ARCHIVE_EXTRACT INPUT "${fsrArchive}" DESTINATION "${HE_FSR31_ROOT}"
            PATTERNS "ffx-api/include/ffx_api/*" "PrebuiltSignedDLL/amd_fidelityfx_dx12.dll"
                "PrebuiltSignedDLL/amd_fidelityfx_vk.dll" "sdk/LICENSE.txt")
        file(WRITE "${HE_FSR31_ROOT}/.he-verified-${fsrHash}" "FidelityFX SDK 1.1.4 / FSR 3.1.4\n")
    endif()
    foreach(fsrRequired IN ITEMS ffx-api/include/ffx_api/ffx_api.h
            PrebuiltSignedDLL/amd_fidelityfx_dx12.dll PrebuiltSignedDLL/amd_fidelityfx_vk.dll sdk/LICENSE.txt)
        if(NOT EXISTS "${HE_FSR31_ROOT}/${fsrRequired}")
            message(FATAL_ERROR "Incomplete FidelityFX SDK: ${HE_FSR31_ROOT}/${fsrRequired}")
        endif()
    endforeach()
    file(READ "${CMAKE_SOURCE_DIR}/Tools/Dependencies/native-runtime.lock.json" fsrRuntimeLock)
    foreach(backend dx12 vk)
        set(name "amd_fidelityfx_${backend}.dll")
        string(JSON expected GET "${fsrRuntimeLock}" files "${name}")
        file(SHA256 "${HE_FSR31_ROOT}/PrebuiltSignedDLL/${name}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "Pinned FSR runtime checksum mismatch: ${name}")
        endif()
        he_require_windows_binary_architecture("${HE_FSR31_ROOT}/PrebuiltSignedDLL/${name}")
    endforeach()
    set(HE_FSR31_RUNTIME_DIR "${HE_FSR31_ROOT}/PrebuiltSignedDLL")
    endif()
endif()

function(he_deploy_fsr31 targetName)
    # Dynamic libraries never appear in the executable's PE import table.
    # Publish a build-authored contract so packaging cannot silently drop them.
    file(GENERATE OUTPUT "$<TARGET_FILE_DIR:${targetName}>/HEngine.Runtime.json" CONTENT
        "{\n  \"schema\": 1,\n  \"architecture\": \"${HE_WINDOWS_ARCH}\",\n  \"fsr\": $<IF:$<BOOL:${HE_FSR31}>,true,false>,\n  \"fsrDx12\": $<IF:$<BOOL:${HE_FSR31_DX12}>,true,false>,\n  \"fsrVulkan\": $<IF:$<BOOL:${HE_FSR31_VULKAN}>,true,false>,\n  \"fsrSourceArchiveSha256\": \"${HE_FSR31_SOURCE_SHA256}\",\n  \"fsrRuntimeSha256\": \"${HE_FSR31_RUNTIME_SHA256}\",\n  \"dx12\": $<IF:$<BOOL:${HE_RENDER_DX12}>,true,false>,\n  \"vulkan\": $<IF:$<BOOL:${HE_RENDER_VULKAN}>,true,false>,\n  \"oidn\": $<IF:$<BOOL:$<TARGET_PROPERTY:${targetName},HE_OIDN_ENABLED>>,true,false>\n}\n")
    if(NOT HE_FSR31)
        return()
    endif()
    he_deploy_vendor_crt(${targetName})
    set(fsrRuntimeFiles "${HE_FSR31_ROOT}/sdk/LICENSE.txt")
    if(HE_FSR31_DX12)
        list(APPEND fsrRuntimeFiles "${HE_FSR31_RUNTIME_DIR}/amd_fidelityfx_dx12.dll")
    endif()
    if(HE_FSR31_VULKAN)
        list(APPEND fsrRuntimeFiles "${HE_FSR31_RUNTIME_DIR}/amd_fidelityfx_vk.dll")
    endif()
    add_custom_target(${targetName}FsrRuntime
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${targetName}>/Licenses"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${HE_FSR31_ROOT}/sdk/LICENSE.txt"
            "$<TARGET_FILE_DIR:${targetName}>/Licenses/FidelityFX.txt"
        DEPENDS ${fsrRuntimeFiles} VERBATIM)
    foreach(fsrBackend IN ITEMS dx12 vk)
        if((fsrBackend STREQUAL "dx12" AND HE_FSR31_DX12) OR
           (fsrBackend STREQUAL "vk" AND HE_FSR31_VULKAN))
            add_custom_command(TARGET ${targetName}FsrRuntime POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${HE_FSR31_RUNTIME_DIR}/amd_fidelityfx_${fsrBackend}.dll"
                    "$<TARGET_FILE_DIR:${targetName}>/amd_fidelityfx_${fsrBackend}.dll" VERBATIM)
        endif()
    endforeach()
    add_dependencies(${targetName} ${targetName}FsrRuntime)
endfunction()
