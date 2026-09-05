cmake_minimum_required(VERSION 3.10)
project(LLGI)

file(GLOB files *.h *.cpp)
file(GLOB files_pc PC/*.h PC/*.cpp)

list(APPEND files ${files_pc})

if(WIN32)
  file(GLOB files_win Win/*.h Win/*.cpp)
  list(APPEND files ${files_win})
elseif(APPLE)
  file(GLOB files_mac Mac/*.h Mac/*.cpp Mac/*.mm)
  list(APPEND files ${files_mac})
elseif(EMSCRIPTEN)
else()
  file(GLOB files_linux Linux/*.h Linux/*.cpp)
  list(APPEND files ${files_linux})
endif()

if(MSVC)
  file(GLOB files_dx12 DX12/*.h DX12/*.cpp)
  list(APPEND files ${files_dx12})

  set(LLGI_DX12_MIPMAP_SHADER_HEADER
      "${CMAKE_CURRENT_BINARY_DIR}/generated/LLGI.DX12MipmapShader.h")
  add_subdirectory("../tools/DX12MipmapShaderCompiler"
                   "${CMAKE_CURRENT_BINARY_DIR}/DX12MipmapShaderCompiler")
  add_custom_command(
    OUTPUT "${LLGI_DX12_MIPMAP_SHADER_HEADER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:LLGI_DX12MipmapShaderCompiler>
            "${LLGI_DX12_MIPMAP_SHADER_HEADER}"
    DEPENDS LLGI_DX12MipmapShaderCompiler
    VERBATIM)
  list(APPEND files "${LLGI_DX12_MIPMAP_SHADER_HEADER}")
endif()

if(BUILD_VULKAN)
  file(GLOB files_vulkan Vulkan/*.h Vulkan/*.cpp)
  list(APPEND files ${files_vulkan})
  add_definitions(-DENABLE_VULKAN)
endif()

if(BUILD_WEBGPU)
  file(GLOB files_webgpu WebGPU/*.h WebGPU/*.cpp)
  list(APPEND files ${files_webgpu})
  add_definitions(-DENABLE_WEBGPU)
endif()

if(APPLE)
  file(GLOB files_metal Metal/*.h Metal/*.cpp Metal/*.mm)
  list(APPEND files ${files_metal})
endif()

foreach(FILE ${files})
  # Get the directory of the source file
  get_filename_component(PARENT_DIR "${FILE}" DIRECTORY)

  # Remove common directory prefix to make the group
  string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}" "" GROUP "${PARENT_DIR}")

  # Make sure we are using windows slashes
  string(REPLACE "/" "\\" GROUP "${GROUP}")

  if("${GROUP}" STREQUAL "")
    set(GROUP "Base")
  endif()

  # Group into "Source Files" and "Header Files"
  if("${FILE}" MATCHES ".*\\.cpp")
    set(GROUP "${GROUP}")
  elseif("${FILE}" MATCHES ".*\\.c")
    set(GROUP "${GROUP}")
  elseif("${FILE}" MATCHES ".*\\.cxx")
    set(GROUP "${GROUP}")
  elseif("${FILE}" MATCHES ".*\\.h")
    set(GROUP "${GROUP}")
  elseif("${FILE}" MATCHES ".*\\.mm")
    set(GROUP "${GROUP}")
  endif()

  source_group("${GROUP}" FILES "${FILE}")
endforeach()

add_library(LLGI STATIC ${files})

if(MSVC)
  target_include_directories(LLGI PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
endif()

file(GLOB LOCAL_HEADERS *.h)
set_target_properties(LLGI PROPERTIES PUBLIC_HEADER "${LOCAL_HEADERS}")
if(BUILD_WEBGPU)
  target_compile_features(LLGI PUBLIC cxx_std_20)
elseif(BUILD_VULKAN)
  target_compile_features(LLGI PUBLIC cxx_std_17)
else()
  target_compile_features(LLGI PUBLIC cxx_std_14)
endif()

if(BUILD_VULKAN)
  find_package(Vulkan REQUIRED)
  target_link_libraries(LLGI PRIVATE ${Vulkan_LIBRARIES})
  if(BUILD_VULKAN_COMPILER AND USE_THIRDPARTY_DIRECTORY)
    target_include_directories(LLGI PRIVATE ${LLGI_THIRDPARTY_INCLUDES} ${Vulkan_INCLUDE_DIRS})
    add_dependencies(LLGI EP_glslang)
  else()
    target_include_directories(LLGI PRIVATE ${Vulkan_INCLUDE_DIRS})
  endif()
endif()

if(BUILD_WEBGPU)
  if(EMSCRIPTEN)
    target_compile_options(LLGI PUBLIC --use-port=emdawnwebgpu)
    target_link_options(LLGI PUBLIC --use-port=emdawnwebgpu)
  elseif(TARGET dawn::webgpu_dawn)
    target_link_libraries(LLGI PUBLIC dawn::webgpu_dawn)
  elseif(TARGET webgpu_dawn)
    target_link_libraries(LLGI PUBLIC webgpu_dawn)
  else()
    message(FATAL_ERROR "Dawn targets were not found. Expected dawn::webgpu_dawn or webgpu_dawn.")
  endif()
endif()

if(WIN32)

elseif(APPLE)

else()
  set_property(TARGET LLGI PROPERTY POSITION_INDEPENDENT_CODE TRUE)
endif()

if(MSVC)
  target_compile_options(LLGI PRIVATE /W4 /WX /wd4100)
else()
  target_compile_options(LLGI PRIVATE -Wall -Werror)
endif()

# --------------------
# Install

install(
  TARGETS LLGI
  EXPORT LLGI-export
  INCLUDES
  DESTINATION include/LLGI
  PUBLIC_HEADER DESTINATION include/LLGI
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib)

install(
  EXPORT LLGI-export
  FILE LLGI-config.cmake
  DESTINATION lib/cmake
  EXPORT_LINK_INTERFACE_LIBRARIES)

clang_format(LLGI)
