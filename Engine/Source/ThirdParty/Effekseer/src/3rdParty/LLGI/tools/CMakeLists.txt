if(BUILD_WEBGPU)
  add_definitions(-DENABLE_WEBGPU)
endif()

if(NOT TARGET ShaderTranspilerCore)
  add_subdirectory(ShaderTranspilerCore)
endif()
add_subdirectory(ShaderTranspiler)
install(TARGETS ShaderTranspiler DESTINATION ${CMAKE_INSTALL_BINDIR})
