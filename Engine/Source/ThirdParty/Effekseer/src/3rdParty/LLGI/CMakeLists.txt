cmake_minimum_required(VERSION 3.15)

# The project's name
project(LLGI)

include(GNUInstallDirs)
include(ExternalProject)
include(CTest)

# linux flag
if(UNIX AND NOT APPLE)
  set(LINUX TRUE)
endif()

option(BUILD_WEBGPU "build webgpu backend" OFF)
option(BUILD_WEBGPU_BROWSER_TEST
       "build a WebAssembly browser smoke test for the WebGPU backend" OFF)
option(BUILD_WEBGPU_BROWSER_SIMD
       "build WebGPU browser tests with WebAssembly SIMD" OFF)
option(BUILD_WEBGPU_BROWSER_THREADS
       "build WebGPU browser tests with WebAssembly threads" OFF)
set(BUILD_WEBGPU_BROWSER_PTHREAD_POOL_SIZE
    "4"
    CACHE STRING "Initial pthread pool size for browser WebGPU tests")
if(BUILD_WEBGPU_BROWSER_TEST AND NOT BUILD_WEBGPU)
  message(FATAL_ERROR "BUILD_WEBGPU_BROWSER_TEST requires BUILD_WEBGPU=ON.")
endif()
if((BUILD_WEBGPU_BROWSER_SIMD OR BUILD_WEBGPU_BROWSER_THREADS)
   AND NOT BUILD_WEBGPU_BROWSER_TEST)
  message(
    FATAL_ERROR
      "BUILD_WEBGPU_BROWSER_SIMD and BUILD_WEBGPU_BROWSER_THREADS require BUILD_WEBGPU_BROWSER_TEST=ON."
  )
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR
   AND BUILD_WEBGPU_BROWSER_TEST
   AND EMSCRIPTEN)
  if(BUILD_WEBGPU_BROWSER_SIMD)
    add_compile_options(-msimd128 -msse2)
    add_link_options(-msimd128 -msse2)
  endif()

  if(BUILD_WEBGPU_BROWSER_THREADS)
    add_compile_options(-pthread)
    add_link_options(-pthread "-sPTHREAD_POOL_SIZE=${BUILD_WEBGPU_BROWSER_PTHREAD_POOL_SIZE}")
  endif()
endif()
set(WEBGPU_DAWN_SOURCE_DIR
    ""
    CACHE PATH "Path to a Dawn source checkout used when BUILD_WEBGPU is ON")
option(WEBGPU_DAWN_BUILD_SAMPLES "Build Dawn sample executables" OFF)
option(WEBGPU_DAWN_FORCE_SYSTEM_COMPONENT_LOAD
       "Let Dawn load required Windows system components from System32"
       ON)
option(BUILD_VULKAN "build vulkan" OFF)
option(BUILD_VULKAN_COMPILER "build vulkan compiler" OFF)
option(BUILD_TEST "build test" OFF)
option(BUILD_EXAMPLE "build examples" OFF)
option(BUILD_TOOL "build tools" OFF)
option(USE_THIRDPARTY_DIRECTORY
       "Whether do it compile with third party directory" ON)

option(GLSLANG_INCLUDE_DIR "A directory which contains glslang includes" "")
option(GLSLANG_WITHOUT_INSTALL "Compile with Glslang without install" OFF)
option(SPIRVCROSS_INCLUDE_DIR "A directory which contains SPIRV-Cross includes"
       "")
option(SPIRVCROSS_WITHOUT_INSTALL "Compile with spirv-cross without install"
       OFF)
option(USE_CREATE_COMPILER_FUNCTION "Whether LLGI::CreateCompiler is used." ON)

if(LINUX AND NOT BUILD_WEBGPU)
  set(BUILD_VULKAN TRUE)
endif()

option(USE_MSVC_RUNTIME_LIBRARY_DLL "compile as multithreaded DLL" ON)

include(cmake/ClangFormat.cmake)

# MSVC Flag
if(MSVC)
  string(PREPEND CMAKE_CXX_FLAGS "/DNOMINMAX ")

  if(POLICY CMP0091)

    cmake_policy(GET CMP0091 CMP0091_STATE)

    if(CMP0091_STATE STREQUAL "NEW")

      if(USE_MSVC_RUNTIME_LIBRARY_DLL)
        cmake_policy(SET CMP0091 NEW)
        set(CMAKE_MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
      else()
        cmake_policy(SET CMP0091 NEW)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
      endif()

    else()

      if(NOT USE_MSVC_RUNTIME_LIBRARY_DLL)
        foreach(flag
                CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
                CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE)

          if(${flag} MATCHES "/MD")
            string(REGEX REPLACE "/MD" "/MT" ${flag} "${${flag}}")
          endif()
          if(${flag} MATCHES "/MDd")
            string(REGEX REPLACE "/MDd" "/MTd" ${flag} "${${flag}}")
          endif()
        endforeach()
      endif()
    endif()

  else()

    if(NOT USE_MSVC_RUNTIME_LIBRARY_DLL)
      foreach(flag
              CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
              CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE)

        if(${flag} MATCHES "/MD")
          string(REGEX REPLACE "/MD" "/MT" ${flag} "${${flag}}")
        endif()
        if(${flag} MATCHES "/MDd")
          string(REGEX REPLACE "/MDd" "/MTd" ${flag} "${${flag}}")
        endif()
      endforeach()
    endif()

  endif()
endif()

if(BUILD_VULKAN_COMPILER OR BUILD_TOOL)
  if(USE_THIRDPARTY_DIRECTORY)

    list(APPEND LLGI_THIRDPARTY_INCLUDES
         ${CMAKE_CURRENT_BINARY_DIR}/EP/Install/glslang/include)
    list(APPEND LLGI_THIRDPARTY_LIBRARY_DIRECTORIES
         ${CMAKE_CURRENT_BINARY_DIR}/EP/Install/glslang/lib)
    set(LLGI_GLSLANG_LIBRARY_DIR
        ${CMAKE_CURRENT_BINARY_DIR}/EP/Install/glslang/lib)

    if ("${CMAKE_BUILD_TYPE}" STREQUAL "" AND NOT WIN32)
    ExternalProject_Add(
      EP_glslang
      SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/glslang
      PREFIX ${CMAKE_CURRENT_BINARY_DIR}/EP/Build/glslang
      INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/EP/Install/glslang
      CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/EP/Install/glslang
        -DCMAKE_USER_MAKE_RULES_OVERRIDE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/CFlagOverrides.cmake
        -DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}
        -DUSE_MSVC_RUNTIME_LIBRARY_DLL=${USE_MSVC_RUNTIME_LIBRARY_DLL}
        -DENABLE_OPT=OFF
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON)

    list(
      APPEND
      LLGI_THIRDPARTY_LIBRARIES
      glslang
      glslang-default-resource-limits
      MachineIndependent
      SPIRV
      OSDependent
      GenericCodeGen)

      else()

      ExternalProject_Add(
      EP_glslang
      SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/glslang
      PREFIX ${CMAKE_CURRENT_BINARY_DIR}/EP/Build/glslang
      INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/EP/Install/glslang
      CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/EP/Install/glslang
        -DCMAKE_USER_MAKE_RULES_OVERRIDE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/CFlagOverrides.cmake
        -DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}
        -DUSE_MSVC_RUNTIME_LIBRARY_DLL=${USE_MSVC_RUNTIME_LIBRARY_DLL}
        -DENABLE_OPT=OFF
        -DCMAKE_DEBUG_POSTFIX=d
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
    list(
      APPEND
      LLGI_THIRDPARTY_LIBRARIES
      debug
      glslangd
      debug
      glslang-default-resource-limitsd
      debug
      MachineIndependentd
      debug
      SPIRVd
      debug
      OSDependentd
      debug
      GenericCodeGend
      optimized
      glslang
      optimized
      glslang-default-resource-limits
      optimized
      MachineIndependent
      optimized
      SPIRV
      optimized
      OSDependent
      optimized
      GenericCodeGen)
    endif()

    ExternalProject_Add(
      EP_SPIRV-Cross
      SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/SPIRV-Cross
      PREFIX ${CMAKE_CURRENT_BINARY_DIR}/EP/Build/SPIRV-Cross
      INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/EP/Install/SPIRV-Cross
      CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/EP/Install/SPIRV-Cross
        -DCMAKE_USER_MAKE_RULES_OVERRIDE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/CFlagOverrides.cmake
        -DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}
        -DUSE_MSVC_RUNTIME_LIBRARY_DLL=${USE_MSVC_RUNTIME_LIBRARY_DLL}
        -DCMAKE_DEBUG_POSTFIX=d
        -DBUILD_SHARED_LIBS=OFF
        -DSPIRV_CROSS_ENABLE_TESTS=OFF
        -DSPIRV_CROSS_FORCE_PIC=ON
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON)

    list(APPEND LLGI_THIRDPARTY_INCLUDES
         ${CMAKE_CURRENT_BINARY_DIR}/EP/Install/SPIRV-Cross/include)
    list(APPEND LLGI_THIRDPARTY_LIBRARY_DIRECTORIES
         ${CMAKE_CURRENT_BINARY_DIR}/EP/Install/SPIRV-Cross/lib)
    set(LLGI_SPIRV_CROSS_LIBRARY_DIR
        ${CMAKE_CURRENT_BINARY_DIR}/EP/Install/SPIRV-Cross/lib)

    list(
      APPEND
      LLGI_THIRDPARTY_LIBRARIES
      debug
      spirv-cross-cored
      debug
      spirv-cross-cd
      debug
      spirv-cross-cppd
      debug
      spirv-cross-hlsld
      debug
      spirv-cross-glsld
      debug
      spirv-cross-msld
      debug
      spirv-cross-reflectd
      debug
      spirv-cross-utild
      optimized
      spirv-cross-core
      optimized
      spirv-cross-c
      optimized
      spirv-cross-cpp
      optimized
      spirv-cross-hlsl
      optimized
      spirv-cross-glsl
      optimized
      spirv-cross-msl
      optimized
      spirv-cross-reflect
      optimized
      spirv-cross-util)

    if(WIN32)
      set(LLGI_THIRDPARTY_LIBRARY_FILES
          debug
          ${LLGI_GLSLANG_LIBRARY_DIR}/glslangd.lib
          debug
          ${LLGI_GLSLANG_LIBRARY_DIR}/glslang-default-resource-limitsd.lib
          debug
          ${LLGI_GLSLANG_LIBRARY_DIR}/MachineIndependentd.lib
          debug
          ${LLGI_GLSLANG_LIBRARY_DIR}/SPIRVd.lib
          debug
          ${LLGI_GLSLANG_LIBRARY_DIR}/OSDependentd.lib
          debug
          ${LLGI_GLSLANG_LIBRARY_DIR}/GenericCodeGend.lib
          optimized
          ${LLGI_GLSLANG_LIBRARY_DIR}/glslang.lib
          optimized
          ${LLGI_GLSLANG_LIBRARY_DIR}/glslang-default-resource-limits.lib
          optimized
          ${LLGI_GLSLANG_LIBRARY_DIR}/MachineIndependent.lib
          optimized
          ${LLGI_GLSLANG_LIBRARY_DIR}/SPIRV.lib
          optimized
          ${LLGI_GLSLANG_LIBRARY_DIR}/OSDependent.lib
          optimized
          ${LLGI_GLSLANG_LIBRARY_DIR}/GenericCodeGen.lib
          debug
          ${LLGI_SPIRV_CROSS_LIBRARY_DIR}/spirv-cross-cored.lib
          debug
          ${LLGI_SPIRV_CROSS_LIBRARY_DIR}/spirv-cross-cd.lib
          debug
          ${LLGI_SPIRV_CROSS_LIBRARY_DIR}/spirv-cross-cppd.lib
          debug
          ${LLGI_SPIRV_CROSS_LIBRARY_DIR}/spirv-cross-hlsld.lib
          debug
          ${LLGI_SPIRV_CROSS_LIBRARY_DIR}/spirv-cross-glsld.lib
          debug
          ${LLGI_SPIRV_CROSS_LIBRARY_DIR}/spirv-cross-msld.lib
          debug
          ${LLGI_SPIRV_CROSS_LIBRARY_DIR}/spirv-cross-reflectd.lib
          debug
          ${LLGI_SPIRV_CROSS_LIBRARY_DIR}/spirv-cross-utild.lib
          optimized
          ${LLGI_SPIRV_CROSS_LIBRARY_DIR}/spirv-cross-core.lib
          optimized
          ${LLGI_SPIRV_CROSS_LIBRARY_DIR}/spirv-cross-c.lib
          optimized
          ${LLGI_SPIRV_CROSS_LIBRARY_DIR}/spirv-cross-cpp.lib
          optimized
          ${LLGI_SPIRV_CROSS_LIBRARY_DIR}/spirv-cross-hlsl.lib
          optimized
          ${LLGI_SPIRV_CROSS_LIBRARY_DIR}/spirv-cross-glsl.lib
          optimized
          ${LLGI_SPIRV_CROSS_LIBRARY_DIR}/spirv-cross-msl.lib
          optimized
          ${LLGI_SPIRV_CROSS_LIBRARY_DIR}/spirv-cross-reflect.lib
          optimized
          ${LLGI_SPIRV_CROSS_LIBRARY_DIR}/spirv-cross-util.lib)
    endif()

  else()
    list(APPEND LLGI_THIRDPARTY_INCLUDES ${GLSLANG_INCLUDE_DIR}
         ${SPIRVCROSS_INCLUDE_DIR})

  endif()

endif()

if(BUILD_VULKAN)
  add_compile_definitions(ENABLE_VULKAN)
endif()

if(BUILD_VULKAN OR BUILD_WEBGPU)
  if(GLSLANG_WITHOUT_INSTALL)
    add_compile_definitions(ENABLE_GLSLANG_WITHOUT_INSTALL)
  endif()

  if(BUILD_VULKAN_COMPILER)
    add_compile_definitions(ENABLE_VULKAN_COMPILER)
  endif()
endif()

if(BUILD_WEBGPU AND NOT EMSCRIPTEN)
  set(DAWN_FETCH_DEPENDENCIES
      OFF
      CACHE BOOL
            "Dawn dependencies should be fetched before configuring LLGI"
            FORCE)
  set(DAWN_BUILD_SAMPLES ${WEBGPU_DAWN_BUILD_SAMPLES}
      CACHE BOOL "Build Dawn samples" FORCE)
  set(DAWN_FORCE_SYSTEM_COMPONENT_LOAD
      ${WEBGPU_DAWN_FORCE_SYSTEM_COMPONENT_LOAD}
      CACHE BOOL "Allow Dawn to load Windows system components from System32"
      FORCE)
  set(DAWN_BUILD_TESTS OFF CACHE BOOL "Build Dawn tests" FORCE)
  set(DAWN_BUILD_PROTOBUF OFF CACHE BOOL "Build Dawn protobuf dependencies" FORCE)
  set(TINT_BUILD_TESTS OFF CACHE BOOL "Build Tint tests" FORCE)
  set(TINT_BUILD_IR_BINARY OFF CACHE BOOL "Build Tint IR binary support" FORCE)
  set(DAWN_ENABLE_INSTALL ON CACHE BOOL "Install Dawn targets" FORCE)
  if(USE_MSVC_RUNTIME_LIBRARY_DLL)
    set(ABSL_MSVC_STATIC_RUNTIME OFF
        CACHE BOOL "Use the static MSVC runtime for Abseil" FORCE)
  else()
    set(ABSL_MSVC_STATIC_RUNTIME ON
        CACHE BOOL "Use the static MSVC runtime for Abseil" FORCE)
  endif()

  set(LLGI_DAWN_SOURCE_DIR "")
  if(WEBGPU_DAWN_SOURCE_DIR)
    set(LLGI_DAWN_SOURCE_DIR "${WEBGPU_DAWN_SOURCE_DIR}")
  elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/dawn/CMakeLists.txt")
    set(LLGI_DAWN_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/dawn")
  endif()

  if(LLGI_DAWN_SOURCE_DIR)
    add_subdirectory("${LLGI_DAWN_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dawn"
                     EXCLUDE_FROM_ALL)
  else()
    message(
      FATAL_ERROR
        "Dawn source was not found. Run `python scripts/fetch_dawn.py`, add thirdparty/dawn, or set WEBGPU_DAWN_SOURCE_DIR to a prepared Dawn checkout."
    )
  endif()
endif()

if(APPLE)
  add_compile_definitions(ENABLE_METAL)
endif()

add_subdirectory("src")

if(BUILD_TEST OR BUILD_WEBGPU_BROWSER_TEST)
  add_subdirectory("src_test")
endif()

if(BUILD_EXAMPLE)
  add_subdirectory("examples")
endif()

if(BUILD_TOOL)
  add_subdirectory("tools")
endif()

if(USE_CREATE_COMPILER_FUNCTION)
  target_compile_definitions(LLGI PRIVATE ENABLE_CREATE_COMPILER)
endif()
