set(StbSourceDir ${ThirdPartyDir}/stb_image)

file(GLOB_RECURSE Box2DSrc 
    "${StbSourceDir}/*.h"
    "${StbSourceDir}/*.cpp"
)

add_library(stb STATIC ${Box2DSrc})
target_include_directories(stb PUBLIC "${StbSourceDir}")