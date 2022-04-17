set(EngineRootDir "${ProjectRootDir}/Engine")
set(EngineAssetsDir "${EngineRootDir}/Assets")
set(EngineShadersDir "${EngineRootDir}/Shaders")
set(EngineSourceDir "${EngineRootDir}/Source")

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "HEngine only supports 64-bit platforms now")
endif ()

if (PROJECT_BINARY_DIR STREQUAL PROJECT_SOURCE_DIR)
    message(WARNING "The binary directory of CMake cannot be the same as source directory!")
endif()

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if (WIN32)
    add_definitions(-DNOMINMAX -D_USE_MATH_DEFINES)
endif()

if (NOT MSVC)
    find_program(CCACHE_PROGRAM ccache)
    if (CCACHE_PROGRAM)
        message(STATUS "Found CCache: ${CCACHE_PROGRAM}")
        set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ${CCACHE_PROGRAM})
        set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ${CCACHE_PROGRAM})
    endif()
endif()

find_package(OpenGL 4.5 REQUIRED)

if(OPENGL_FOUND)
    message("Found OpenGL")
else()
    message("Not Found OpenGL")
endif()

file(COPY ${EngineAssetsDir}
DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

file(COPY ${EngineShadersDir}
DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

add_subdirectory(Source)