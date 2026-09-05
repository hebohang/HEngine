cmake_minimum_required(VERSION 3.10)
project(ShaderTranspilerCore)

add_library(
  ShaderTranspilerCore STATIC ShaderTranspilerCore.cpp ShaderTranspilerCore.h)

if(BUILD_WEBGPU)
  target_compile_features(ShaderTranspilerCore PUBLIC cxx_std_20)
else()
  target_compile_features(ShaderTranspilerCore PUBLIC cxx_std_17)
endif()
target_include_directories(
  ShaderTranspilerCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                              ${LLGI_THIRDPARTY_INCLUDES})

if(USE_THIRDPARTY_DIRECTORY)
  if(LLGI_THIRDPARTY_LIBRARY_FILES)
    target_link_libraries(ShaderTranspilerCore
                          PUBLIC ${LLGI_THIRDPARTY_LIBRARY_FILES})
  else()
    target_link_libraries(ShaderTranspilerCore
                          PUBLIC ${LLGI_THIRDPARTY_LIBRARIES})
  endif()
  target_link_directories(ShaderTranspilerCore PUBLIC
                          ${LLGI_THIRDPARTY_LIBRARY_DIRECTORIES})
endif()

if(WIN32)
  # None
else()
  find_package(Threads REQUIRED)
  target_link_libraries(ShaderTranspilerCore PUBLIC ${CMAKE_THREAD_LIBS_INIT}
                                                    pthread)

endif()

if(USE_THIRDPARTY_DIRECTORY)
  if(TARGET EP_glslang)
    add_dependencies(ShaderTranspilerCore EP_glslang)
  endif()
  if(TARGET EP_SPIRV-Cross)
    add_dependencies(ShaderTranspilerCore EP_SPIRV-Cross)
  endif()
endif()

if(MSVC)
  target_compile_options(ShaderTranspilerCore PRIVATE /W4 /WX /wd4100 /wd4324)
else()
  target_compile_options(ShaderTranspilerCore PRIVATE -Wall -Werror)
endif()

if(GLSLANG_WITHOUT_INSTALL)
  target_compile_definitions(ShaderTranspilerCore PRIVATE ENABLE_GLSLANG_WITHOUT_INSTALL)
endif()

if(SPIRVCROSS_WITHOUT_INSTALL)
  target_compile_definitions(ShaderTranspilerCore PRIVATE ENABLE_SPIRVCROSS_WITHOUT_INSTALL)
endif()

if(BUILD_WEBGPU)
  target_compile_definitions(ShaderTranspilerCore PRIVATE ENABLE_WEBGPU)
  target_link_libraries(ShaderTranspilerCore PRIVATE tint_api)
endif()
