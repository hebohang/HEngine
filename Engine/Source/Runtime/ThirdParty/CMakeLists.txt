add_subdirectory(Glad)
add_subdirectory(GLFW)
add_subdirectory(imgui)

if(NOT TARGET yaml-cpp)
    include(yaml-cpp.cmake)
endif()