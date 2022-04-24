set(bullet3SourceDir ${ThirdPartyDir}/bullet3)

file(GLOB_RECURSE Box2DSrc 
    "${bullet3SourceDir}/src/Bullet3Collision/*.cpp"
    "${bullet3SourceDir}/src/*.cpp"
)

add_library(Box2D STATIC ${Box2DSrc})
target_include_directories(Box2D PUBLIC "${Box2DSourceDir}/include")