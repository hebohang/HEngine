set(StbSourceDir ${ThirdPartyDir}/stb_image)

set(stbSrc 
    "${StbSourceDir}/stb_image.h"
    "${StbSourceDir}/stb_image.cpp"
)

add_library(stb STATIC ${stbSrc})
target_include_directories(stb PUBLIC "${StbSourceDir}")