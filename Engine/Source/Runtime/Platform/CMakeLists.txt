add_subdirectory(DirectX11)
add_subdirectory(OpenGL)
add_subdirectory(Vulkan)
add_subdirectory(Windows)

# PARENT_SCOPE only one level
# this command is to pass the variable to parent
RUNTIME_ADD_SOURCE()