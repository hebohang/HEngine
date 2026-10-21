cmake_minimum_required(VERSION 4.2)
# --version can succeed even when an interrupted installation lost Modules/.
# Exercise script mode, which is what the source-free exporters actually use.
if(NOT EXISTS "${CMAKE_ROOT}/Modules/CMake.cmake")
    message(FATAL_ERROR "CMake module tree is incomplete; reinstall the pinned SDK tool")
endif()
include(CMakeParseArguments)
message(STATUS "SDK CMake script-mode and module tree PASS")
