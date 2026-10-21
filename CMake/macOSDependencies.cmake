# Import native SDKs before the legacy Windows-only vendor wrappers are visited.
# Keep the engine's source-built dependencies and pinned Vulkan headers shared
# with the other platforms.
if(HE_WITH_EDITOR)
    find_package(embree 4 REQUIRED CONFIG)
endif()
find_package(Vulkan REQUIRED)

# The OS-provided zlib has the same archive ABI and remains available to
# standalone applications. The legacy vendored configuration targets Classic
# Mac OS and cannot be used with current Apple SDK headers.
find_package(ZLIB REQUIRED)
if(NOT TARGET zlibstatic)
    add_library(zlibstatic INTERFACE)
    target_link_libraries(zlibstatic INTERFACE ZLIB::ZLIB)
endif()
