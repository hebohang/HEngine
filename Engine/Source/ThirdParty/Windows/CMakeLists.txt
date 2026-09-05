add_library(DirectX INTERFACE)
# add_subdirectory(DirectXTex)
target_link_libraries(DirectX INTERFACE 
    # DirectXTex
    Microsoft::DirectX-Headers
    d3d12
    dxgi
    dxguid
)
