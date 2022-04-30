set(ThirdPartyDir "${EngineSourceDir}/ThirdParty")
set(EngineRuntimeDir "${EngineSourceDir}/Runtime")
set(EngineEditorDir "${EngineSourceDir}/Editor")

# from https://github.com/BoomingTech/Pilot
set(vulkan_include ${ThirdPartyDir}/VulkanSDK/include)

if(WIN32)
    set(vulkan_lib ${ThirdPartyDir}/VulkanSDK/lib/Win32/vulkan-1.lib)
    set(glslangValidator_executable ${ThirdPartyDir}/VulkanSDK/bin/Win32/glslangValidator.exe)
elseif(UNIX)
    if(APPLE)
      set(vulkan_lib ${ThirdPartyDir}/VulkanSDK/lib/MacOS/libvulkan.1.dylib)
      set(glslangValidator_executable ${ThirdPartyDir}/VulkanSDK/bin/MacOS/glslangValidator)
    else()
      set(vulkan_lib ${ThirdPartyDir}/VulkanSDK/lib/Linux/libvulkan.so.1)
      set(glslangValidator_executable ${ThirdPartyDir}/VulkanSDK/bin/Linux/glslangValidator)
    endif()
else()
  message(FATAL_ERROR "Unknown Platform")
endif()
# end from

add_subdirectory(ThirdParty)

list(APPEND CMAKE_MODULE_PATH 
  "${PROJECT_SOURCE_DIR}/CMake"
  "${PROJECT_SOURCE_DIR}/CMake/Utils"
  "${PROJECT_SOURCE_DIR}/CMake/Packages"
)

if (WIN32)
    find_package(DirectX11)
endif()

add_subdirectory(Runtime)
add_subdirectory(Editor)

if(MSVC)
    set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT HEngineEditor)
endif()