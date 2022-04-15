set(Box2DSourceDir ${CMAKE_CURRENT_SOURCE_DIR}/Box2D)

file(GLOB_RECURSE Box2DSrc 
    "${Box2DSourceDir}/include/*.h"
    "${Box2DSourceDir}/src/*.cpp"
)

add_library(Box2D STATIC ${Box2DSrc})
target_include_directories(Box2D PUBLIC "${Box2DSourceDir}/include")