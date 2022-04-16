set(Box2DSourceDir ${ThirdPartyDir}/Box2D)

file(GLOB_RECURSE Box2DSrc 
    "${Box2DSourceDir}/include/*.h"
    "${Box2DSourceDir}/src/*.cpp"
)

add_library(Box2D STATIC ${Box2DSrc})
target_include_directories(Box2D PUBLIC "${Box2DSourceDir}/include")