add_subdirectory(Audio)
if(HE_WITH_EDITOR)
    add_subdirectory(Embree)
endif()
add_subdirectory(Interop)
if(HE_RENDER_DX12)
    add_subdirectory(DirectX)
endif()
if(HE_RENDER_VULKAN)
    add_subdirectory(Vulkan)
endif()

if(ANDROID)
    add_subdirectory(Android)
elseif(APPLE)
    add_subdirectory(macOS)
    add_subdirectory(GLFW)
else()
    add_subdirectory(Windows)
    add_subdirectory(GLFW)
endif()

# TODO: Windows use windows native api to create window
# if (WIN32)
#     add_subdirectory(Windows)
# else()
#     add_subdirectory(GLFW)
# endif()

# PARENT_SCOPE only one level
# this command is to pass the variable to parent
set(platformSources
    Scripting/ScriptPlatformServices.h Scripting/PcScriptServices.cpp
    Scripting/ScriptBodyControl.cpp
    PlatformFileSystem.h
    PlatformPaths.h
    PlatformProcess.h
    RenderBackendFactory.cpp
)

if(HE_ENABLE_PHYSICS_3D)
    list(APPEND platformSources PhysicsCookedMesh.h PhysicsCookedMesh.cpp)
endif()
RUNTIME_ADD_SOURCE(${platformSources})
