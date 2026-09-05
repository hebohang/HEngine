add_subdirectory(Audio)
add_subdirectory(Embree)
add_subdirectory(Interop)
add_subdirectory(DirectX)
if(HE_RENDER_VULKAN)
    add_subdirectory(Vulkan)
endif()

add_subdirectory(Windows)
add_subdirectory(GLFW)

# TODO: Windows use windows native api to create window
# if (WIN32)
#     add_subdirectory(Windows)
# else()
#     add_subdirectory(GLFW)
# endif()

# PARENT_SCOPE only one level
# this command is to pass the variable to parent
RUNTIME_ADD_SOURCE(
    PlatformFileSystem.h
    PlatformPaths.h
    PlatformProcess.h
    RenderBackendFactory.cpp
)
