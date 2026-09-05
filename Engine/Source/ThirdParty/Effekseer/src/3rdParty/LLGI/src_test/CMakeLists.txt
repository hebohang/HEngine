if(BUILD_WEBGPU_BROWSER_TEST)
  set(files
      main.cpp
      RenderTestHelper.cpp
      RenderTestHelper.h
      TestHelper.cpp
      TestHelper.h
      TextureDataGenerator.cpp
      TextureDataGenerator.h
      TextureRenderTestHelper.cpp
      TextureRenderTestHelper.h
      test.h
      test_webgpu_browser.cpp)
else()
  file(GLOB files CONFIGURE_DEPENDS *.h *.cpp)
  list(FILTER files EXCLUDE REGEX "test_webgpu_browser\\.cpp$")
endif()

add_executable(LLGI_Test ${files})

if(BUILD_WEBGPU_BROWSER_TEST AND NOT EMSCRIPTEN)
  message(FATAL_ERROR "BUILD_WEBGPU_BROWSER_TEST requires an Emscripten CMake toolchain.")
endif()

if(APPLE)

  find_library(COCOA_LIBRARY Cocoa)
  find_library(METAL_LIBRARY Metal)
  find_library(APPKIT_LIBRARY AppKit)
  find_library(METALKIT_LIBRARY MetalKit)
  find_library(QUARTZ_CORE_LIBRARY QuartzCore)

  set(EXTRA_LIBS ${COCOA_LIBRARY} ${APPKIT_LIBRARY} ${METAL_LIBRARY}
                 ${METALKIT_LIBRARY} ${QUARTZ_CORE_LIBRARY})
  target_link_libraries(LLGI_Test PRIVATE ${EXTRA_LIBS})

endif()

target_include_directories(LLGI_Test PUBLIC ../src/)

target_link_libraries(LLGI_Test PRIVATE LLGI)
target_compile_features(LLGI_Test PUBLIC cxx_std_14)

if(BUILD_VULKAN_COMPILER AND USE_THIRDPARTY_DIRECTORY)

  target_link_directories(LLGI_Test PRIVATE
                          ${LLGI_THIRDPARTY_LIBRARY_DIRECTORIES})
  target_link_libraries(LLGI_Test PRIVATE ${LLGI_THIRDPARTY_LIBRARIES})
  add_dependencies(LLGI_Test EP_glslang EP_SPIRV-Cross)
endif()

if(BUILD_WEBGPU_BROWSER_TEST)
  set_target_properties(LLGI_Test PROPERTIES SUFFIX ".html")
  target_link_options(
    LLGI_Test
    PRIVATE
    "--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/browser/pre_webgpu_test.js"
    "--preload-file=${CMAKE_CURRENT_SOURCE_DIR}/Shaders/WebGPU@/Shaders/WebGPU"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sASYNCIFY=1"
    "-sASSERTIONS=1")
elseif(MSVC)
  target_link_libraries(LLGI_Test PRIVATE)
elseif(APPLE)
  target_link_libraries(LLGI_Test PRIVATE)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(LLGI_Test PRIVATE ${CMAKE_THREAD_LIBS_INIT} pthread X11
                                          X11-xcb)
endif()

if(NOT EMSCRIPTEN)
  file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/Shaders
       DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/)
endif()

clang_format(LLGI_Test)

if(EMSCRIPTEN)
elseif(MSVC)
  target_compile_options(LLGI_Test PRIVATE /W4 /WX /wd4100)
else()
  target_compile_options(LLGI_Test PRIVATE -Wall -Werror)
endif()

if(BUILD_WEBGPU_BROWSER_TEST AND EMSCRIPTEN)
  find_program(NODE_EXE node)
  if(NODE_EXE)
    set(llgi_webgpu_browser_test_command
        ${NODE_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/browser/run_webgpu_browser_test.mjs
        $<TARGET_FILE:LLGI_Test> --filter=WebGPUBrowser.*)
    if(BUILD_WEBGPU_BROWSER_THREADS)
      list(APPEND llgi_webgpu_browser_test_command --cross-origin-isolated)
    endif()
    add_test(
      NAME LLGI_WebGPU_Browser
      COMMAND ${llgi_webgpu_browser_test_command})
  endif()
endif()
