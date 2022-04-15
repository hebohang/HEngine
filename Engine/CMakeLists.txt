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

#设置输出目录
#例如Debug-Windows-x64
#set(outputdir "${CMAKE_BUILD_TYPE}-${CMAKE_SYSTEM_NAME}-${CMAKE_GENERATOR_PLATFORM}")
#set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin/${outputdir}/${PROJECT_NAME})
#set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin/${outputdir}/${PROJECT_NAME})
#set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin/${outputdir}/${PROJECT_NAME})

#add_subdirectory(HEngine)
#add_subdirectory(HEngineEditor)
#
#target_compile_definitions(HEngine
#    PRIVATE _CRT_SECURE_NO_WARNINGS
#)
#target_compile_features(HEngine
#    PUBLIC cxx_std_17
#)

find_package(OpenGL 4.5 REQUIRED)

if(OPENGL_FOUND)
    message("Found OpenGL")
else()
    message("Not Found OpenGL")
endif()

add_subdirectory(Source)