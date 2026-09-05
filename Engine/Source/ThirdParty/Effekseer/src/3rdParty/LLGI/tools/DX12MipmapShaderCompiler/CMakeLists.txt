add_executable(LLGI_DX12MipmapShaderCompiler main.cpp)
target_link_libraries(LLGI_DX12MipmapShaderCompiler PRIVATE d3dcompiler)

if(MSVC)
  target_compile_options(LLGI_DX12MipmapShaderCompiler PRIVATE /W4 /WX /wd4100)
endif()
