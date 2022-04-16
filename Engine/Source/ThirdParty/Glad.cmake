set(GladSourceDir ${ThirdPartyDir}/Glad)

file(GLOB GladSrc 
    "${GladSourceDir}/include/glad/glad.h"
    "${GladSourceDir}/include/KHR/khrplatform.h"
    "${GladSourceDir}/src/glad.c"
)

add_library(Glad STATIC ${GladSrc})
target_include_directories(Glad PUBLIC "${GladSourceDir}/include")