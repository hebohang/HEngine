add_subdirectory(DX12)
add_subdirectory(Vulkan)

# Propagate sources collected by the nested platform implementation to the
# Runtime target's parent scope.
RUNTIME_ADD_SOURCE(
    EffekseerRenderInterop.h EffekseerRenderInterop.cpp
)
