if(HE_RENDER_DX12)
    add_subdirectory(DX12)
endif()
if(HE_RENDER_VULKAN)
    add_subdirectory(Vulkan)
endif()

# Propagate sources collected by the nested platform implementation to the
# Runtime target's parent scope.
RUNTIME_ADD_SOURCE(
    EffekseerRenderInterop.h EffekseerRenderInterop.cpp
    Fsr31Interop.cpp
)
