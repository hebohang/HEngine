file(GLOB_RECURSE yaml-cpp_SRCS	
    "${CMAKE_CURRENT_SOURCE_DIR}/yaml-cpp/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/yaml-cpp/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/yaml-cpp/include/*.h"
)
add_library(yaml-cpp STATIC ${yaml-cpp_SRCS})
target_include_directories(yaml-cpp PUBLIC "${ThirdPartyDir}/yaml-cpp/include")
target_compile_definitions(yaml-cpp PUBLIC "YAML_CPP_STATIC_DEFINE")
