cmake_minimum_required(VERSION 3.20)

if(NOT TARGET Vulkan-Headers)
    add_library(Vulkan-Headers INTERFACE)
    add_library(Vulkan::Headers ALIAS Vulkan-Headers)
    target_include_directories(Vulkan-Headers INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
endif()
