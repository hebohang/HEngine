ADD_PLUGIN(OfflineReferenceRenderer)

PLUGIN_ADD_SOURCE(
    OfflineReferenceRendererPluginEntry.cpp
    Source/OfflineReferenceRendererSession.h
    Source/OfflineReferenceRendererSession.cpp
    Source/OfflineReferenceRendererSettingsJson.h
    Source/OfflineReferenceRendererSettingsJson.cpp
    Source/ProgressiveSchedule.h
    Source/ProgressiveDenoiser.h
    Source/ProgressiveDenoiser.cpp
    Tests/OfflineReferenceRendererSessionTests.h
)

set_property(TARGET OfflineReferenceRenderer PROPERTY HE_PLUGIN_SHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/HardwareRayTracingReference.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/OfflineReferencePreview.hlsl"
)

target_include_directories(OfflineReferenceRenderer PRIVATE
    "${EngineRuntimeDir}"
)

target_link_libraries(OfflineReferenceRenderer PRIVATE
    glm
    nvrhi
    nlohmann_json::nlohmann_json
    spdlog
)

END_PLUGIN()

option(HE_OFFLINE_OIDN "Enable the offline plugin's CPU Open Image Denoise backend" ON)
if(HE_OFFLINE_OIDN)
    if(APPLE)
        find_package(OpenImageDenoise 2.5.1 EXACT REQUIRED CONFIG)
        get_target_property(oidnLibrary OpenImageDenoise IMPORTED_LOCATION_RELEASE)
        if(NOT oidnLibrary)
            get_target_property(oidnLibrary OpenImageDenoise IMPORTED_LOCATION)
        endif()
        get_filename_component(oidnLibraryDirectory "${oidnLibrary}" DIRECTORY)
        get_filename_component(HE_OIDN_ROOT "${oidnLibraryDirectory}/.." ABSOLUTE)
        target_include_directories(OfflineReferenceRenderer PRIVATE
            "$<TARGET_PROPERTY:OpenImageDenoise,INTERFACE_INCLUDE_DIRECTORIES>")
        target_sources(OfflineReferenceRenderer PRIVATE
            "${EngineRuntimeDir}/Platform/Interop/OidnDenoiser.cpp")
        target_compile_definitions(OfflineReferenceRenderer PRIVATE HE_OFFLINE_OIDN=1)
        set(oidnRuntime "")
        foreach(name libOpenImageDenoise.2.5.1.dylib libOpenImageDenoise_core.2.5.1.dylib libOpenImageDenoise_device_cpu.2.5.1.dylib)
            if(NOT EXISTS "${oidnLibraryDirectory}/${name}")
                message(FATAL_ERROR "Incomplete macOS OIDN CPU runtime: ${name}")
            endif()
            list(APPEND oidnRuntime "${oidnLibraryDirectory}/${name}")
        endforeach()
        set_property(TARGET OfflineReferenceRenderer PROPERTY HE_PLUGIN_RUNTIME_FILES "${oidnRuntime}")
        set_property(TARGET OfflineReferenceRenderer PROPERTY HE_PLUGIN_RUNTIME_DIRECTORY "Plugins/OfflineReferenceRenderer")
        set_property(TARGET OfflineReferenceRenderer PROPERTY HE_OIDN_ENABLED TRUE)
        set_property(TARGET OfflineReferenceRenderer PROPERTY HE_PLUGIN_LICENSE_FILES
            "${HE_OIDN_ROOT}/share/doc/OpenImageDenoise/LICENSE.txt"
            "${HE_OIDN_ROOT}/share/doc/OpenImageDenoise/third-party-programs.txt"
            "${HE_OIDN_ROOT}/share/doc/OpenImageDenoise/third-party-programs-oneTBB.txt")
    else()
    set(HE_OIDN_ROOT "${RootDir}/out/agent/dependencies/windows/oidn" CACHE PATH
        "Prepared OIDN 2.5.1 SDK; run hectl deps install -Profile Editor.")
    set(he_oidn_SOURCE_DIR "${HE_OIDN_ROOT}")
    find_package(OpenImageDenoise 2.5.1 EXACT REQUIRED CONFIG
        PATHS "${he_oidn_SOURCE_DIR}/lib/cmake/OpenImageDenoise-2.5.1" NO_DEFAULT_PATH)
    # CPU-only, loaded on first denoise. No OIDN import in the Editor executable.
    target_include_directories(OfflineReferenceRenderer PRIVATE "${HE_OIDN_ROOT}/include")
    target_sources(OfflineReferenceRenderer PRIVATE
        "${EngineRuntimeDir}/Platform/Interop/OidnDenoiser.cpp")
    target_compile_definitions(OfflineReferenceRenderer PRIVATE HE_OFFLINE_OIDN=1)
    set(oidnRuntime "")
    file(READ "${RootDir}/Tools/Dependencies/native-runtime.lock.json" oidnRuntimeLock)
    foreach(name OpenImageDenoise.dll OpenImageDenoise_core.dll OpenImageDenoise_device_cpu.dll)
        if(NOT EXISTS "${HE_OIDN_ROOT}/bin/${name}")
            message(FATAL_ERROR "Incomplete OIDN CPU runtime: ${name}")
        endif()
        list(APPEND oidnRuntime "${HE_OIDN_ROOT}/bin/${name}")
        string(JSON expected GET "${oidnRuntimeLock}" files "Plugins/OfflineReferenceRenderer/${name}")
        file(SHA256 "${HE_OIDN_ROOT}/bin/${name}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "Pinned OIDN runtime checksum mismatch: ${name}")
        endif()
        he_require_windows_binary_architecture("${HE_OIDN_ROOT}/bin/${name}")
    endforeach()
    set_property(TARGET OfflineReferenceRenderer PROPERTY HE_PLUGIN_RUNTIME_FILES "${oidnRuntime}")
    set_property(TARGET OfflineReferenceRenderer PROPERTY HE_PLUGIN_RUNTIME_DIRECTORY "Plugins/OfflineReferenceRenderer")
    # Embree and OIDN must use one app-local oneTBB, never competing copies.
    set_property(TARGET OfflineReferenceRenderer PROPERTY HE_PLUGIN_SHARED_RUNTIME_FILES "${HE_OIDN_ROOT}/bin/tbb12.dll")
    string(JSON expected GET "${oidnRuntimeLock}" files "tbb12.dll")
    file(SHA256 "${HE_OIDN_ROOT}/bin/tbb12.dll" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Pinned OIDN oneTBB runtime checksum mismatch")
    endif()
    set_property(TARGET OfflineReferenceRenderer PROPERTY HE_OIDN_ENABLED TRUE)
    set(oidnLicenses "${HE_OIDN_ROOT}/doc/LICENSE.txt"
        "${HE_OIDN_ROOT}/doc/third-party-programs.txt"
        "${HE_OIDN_ROOT}/doc/third-party-programs-oneTBB.txt")
    set_property(TARGET OfflineReferenceRenderer PROPERTY HE_PLUGIN_LICENSE_FILES "${oidnLicenses}")
    endif()
else()
    target_compile_definitions(OfflineReferenceRenderer PRIVATE HE_OFFLINE_OIDN=0)
endif()
