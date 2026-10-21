# HEngine's Windows Vulkan build is intentionally independent of an installed
# Vulkan SDK. The loader ABI is stable; compilation uses the pinned headers and
# linking uses the vendored x64 import library. The runtime DLL comes only from
# the installed graphics driver and is delay-loaded by the final executable.

if(APPLE)
    set(HE_MOLTENVK_ROOT "/opt/homebrew/opt/molten-vk" CACHE PATH "MoltenVK SDK installation")
    find_library(Vulkan_LIBRARY NAMES MoltenVK
        HINTS "${HE_MOLTENVK_ROOT}/lib" "${HE_MOLTENVK_ROOT}/MoltenVK/dylib/macOS" REQUIRED)
    set(Vulkan_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/Vulkan-Headers/include")
    set(Vulkan_INCLUDE_DIRS "${Vulkan_INCLUDE_DIR}")
    set(Vulkan_LIBRARIES "${Vulkan_LIBRARY}")
    set(Vulkan_VERSION "1.4.352")
    set(Vulkan_FOUND TRUE)
    if(NOT TARGET Vulkan::Vulkan)
        add_library(Vulkan::Vulkan UNKNOWN IMPORTED GLOBAL)
        set_target_properties(Vulkan::Vulkan PROPERTIES
            IMPORTED_LOCATION "${Vulkan_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${Vulkan_INCLUDE_DIR}")
    endif()
    return()
endif()

if(ANDROID)
    # Use NDK libraries for the target ABI; never a Windows SDK import library.
    find_library(Vulkan_LIBRARY NAMES vulkan REQUIRED)
    set(Vulkan_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/Vulkan-Headers/include")
    set(Vulkan_INCLUDE_DIRS "${Vulkan_INCLUDE_DIR}")
    set(Vulkan_LIBRARIES "${Vulkan_LIBRARY}")
    set(Vulkan_VERSION "1.4.352")
    set(Vulkan_FOUND TRUE)
    if(NOT TARGET Vulkan::Vulkan)
        add_library(Vulkan::Vulkan UNKNOWN IMPORTED GLOBAL)
        set_target_properties(Vulkan::Vulkan PROPERTIES IMPORTED_LOCATION "${Vulkan_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${Vulkan_INCLUDE_DIR}")
    endif()
    return()
endif()

if(NOT WIN32)
    set(Vulkan_FOUND FALSE)
    if(Vulkan_FIND_REQUIRED)
        message(FATAL_ERROR "HEngine's hermetic Vulkan module currently supports Windows only")
    endif()
    return()
endif()

set(Vulkan_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/Vulkan-Headers/include")
set(Vulkan_INCLUDE_DIRS "${Vulkan_INCLUDE_DIR}")
set(Vulkan_LIBRARY "${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/VulkanSDK/lib/Win32/vulkan-1.lib")
if(HE_WINDOWS_ARCH STREQUAL "arm64")
    set(Vulkan_LIBRARY "${HE_WINDOWS_ARM64_ROOT}/vulkan/lib/vulkan-1.lib")
endif()
set(Vulkan_LIBRARIES "${Vulkan_LIBRARY}")
set(Vulkan_VERSION "1.4.352")

if(NOT EXISTS "${Vulkan_INCLUDE_DIR}/vulkan/vulkan_core.h" OR
   NOT EXISTS "${Vulkan_LIBRARY}")
    set(Vulkan_FOUND FALSE)
else()
    set(Vulkan_FOUND TRUE)
endif()

if(Vulkan_FOUND AND NOT TARGET Vulkan::Vulkan)
    add_library(Vulkan::Vulkan UNKNOWN IMPORTED GLOBAL)
    set_target_properties(Vulkan::Vulkan PROPERTIES
        IMPORTED_LOCATION "${Vulkan_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${Vulkan_INCLUDE_DIR}")
endif()

if(Vulkan_FOUND AND NOT TARGET Vulkan::Headers AND NOT TARGET Vulkan-Headers)
    add_library(Vulkan-Headers INTERFACE)
    add_library(Vulkan::Headers ALIAS Vulkan-Headers)
    target_include_directories(Vulkan-Headers INTERFACE "${Vulkan_INCLUDE_DIR}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Vulkan
    REQUIRED_VARS Vulkan_INCLUDE_DIR Vulkan_LIBRARY
    VERSION_VAR Vulkan_VERSION)
