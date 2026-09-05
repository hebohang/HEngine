set(StbSourceDir ${ThirdPartyDir}/stb_image)

set(stbSrc 
    "${StbSourceDir}/stb_image.h"
    "${StbSourceDir}/stb_image.cpp"
    "${StbSourceDir}/stb_image_resize2.h"
    "${StbSourceDir}/stb_image_resize2.cpp"
)

add_library(stb STATIC ${stbSrc})
target_include_directories(stb PUBLIC "${StbSourceDir}")
