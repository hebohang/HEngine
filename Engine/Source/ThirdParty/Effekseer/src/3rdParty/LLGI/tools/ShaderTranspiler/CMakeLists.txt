cmake_minimum_required(VERSION 3.10)
project(ShaderTranspiler)

add_executable(ShaderTranspiler main.cpp)

if(BUILD_WEBGPU)
  target_compile_features(ShaderTranspiler PUBLIC cxx_std_20)
else()
  target_compile_features(ShaderTranspiler PUBLIC cxx_std_17)
endif()

target_include_directories(ShaderTranspiler PUBLIC ../ShaderTranspilerCore)

target_link_libraries(ShaderTranspiler PUBLIC ShaderTranspilerCore)

if(MSVC)
  target_compile_options(ShaderTranspiler PRIVATE /W4 /WX /wd4100)
else()
  target_compile_options(ShaderTranspiler PRIVATE -Wall -Werror)
endif()
