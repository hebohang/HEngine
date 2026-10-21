RUNTIME_ADD_SOURCE(MacInputEvents.h)

set(macPlatformSources
    "${CMAKE_CURRENT_SOURCE_DIR}/MacPlatformFileSystem.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/MacPlatformProcess.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/MacPlatformPaths.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/MacPlatformUtils.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/MacInputEvents.mm")
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/MacPlatformPaths.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/MacPlatformUtils.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/MacInputEvents.mm"
    DIRECTORY "${EngineRuntimeDir}" PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_OPTIONS "-fobjc-arc;-std=c++20")
set(RuntimeSourceFiles ${RuntimeSourceFiles} ${macPlatformSources} PARENT_SCOPE)

# Isolated native contracts run without a window or graphics device.
add_executable(HEMacPlatformContracts EXCLUDE_FROM_ALL
    Tests/MacPlatformContracts.cpp
    MacPlatformFileSystem.cpp
    MacPlatformProcess.cpp)
target_compile_features(HEMacPlatformContracts PRIVATE cxx_std_20)
target_compile_options(HEMacPlatformContracts PRIVATE -UNDEBUG)
target_include_directories(HEMacPlatformContracts PRIVATE "${EngineSourceDir}")
set_target_properties(HEMacPlatformContracts PROPERTIES FOLDER "Tests")

# Exercise Cocoa's logical event location independently of the live cursor,
# including wrong-window rejection and the callback's scoped override lifetime.
add_executable(HEMacInputEventContracts EXCLUDE_FROM_ALL
    Tests/MacInputEventContracts.mm
    MacInputEvents.mm
    ../GLFW/GlfwInputEvents.cpp
    ../../Core/Log/Log.cpp)
target_compile_features(HEMacInputEventContracts PRIVATE cxx_std_20)
target_compile_options(HEMacInputEventContracts PRIVATE -UNDEBUG
    "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
target_compile_definitions(HEMacInputEventContracts PRIVATE GLFW_INCLUDE_NONE WITH_EDITOR=1)
target_include_directories(HEMacInputEventContracts PRIVATE "${EngineSourceDir}" "${EngineRuntimeDir}")
target_link_libraries(HEMacInputEventContracts PRIVATE glfw glm spdlog "-framework Cocoa")
set_target_properties(HEMacInputEventContracts PROPERTIES FOLDER "Tests" OBJCXX_STANDARD 20)
