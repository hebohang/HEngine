add_library(DirectXTex INTERFACE)
target_include_directories(DirectXTex INTERFACE "${ThirdPartyDir}/Windows/DirectXTex")
target_sources(DirectXTex INTERFACE 
    ${ThirdPartyDir}/Windows/DirectXTex/DDSTextureLoader/DDSTextureLoader11.cpp
    ${ThirdPartyDir}/Windows/DirectXTex/DDSTextureLoader/DDSTextureLoader11.h
    ${ThirdPartyDir}/Windows/DirectXTex/WICTextureLoader/WICTextureLoader11.cpp
    ${ThirdPartyDir}/Windows/DirectXTex/WICTextureLoader/WICTextureLoader11.h
    # ${ThirdPartyDir}/Windows/DirectXTex/DDSTextureLoader/DDSTextureLoader12.cpp
    # ${ThirdPartyDir}/Windows/DirectXTex/DDSTextureLoader/DDSTextureLoader12.h
    # ${ThirdPartyDir}/Windows/DirectXTex/WICTextureLoader/WICTextureLoader12.cpp
    # ${ThirdPartyDir}/Windows/DirectXTex/WICTextureLoader/WICTextureLoader12.h
)
