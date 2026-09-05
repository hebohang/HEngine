#ifndef HE_BACKEND_PUSH_CONSTANTS_HLSLI
#define HE_BACKEND_PUSH_CONSTANTS_HLSLI

// NVRHI maps root/push constants to a b-register on D3D12, while Vulkan
// requires SPIR-V PushConstant storage and creates no descriptor for the
// binding-layout item. HShaderCompiler defines HE_SPIRV only for its Vulkan
// artifact target so one HLSL source preserves the same byte layout on both.
#if defined(HE_SPIRV)
#define HE_DECLARE_PUSH_CONSTANTS_AT(typeName, variableName, registerIndex, registerSpace) \
    [[vk::push_constant]] ConstantBuffer<typeName> variableName
#else
#define HE_PUSH_CONSTANT_REGISTER(registerIndex) b##registerIndex
#define HE_DECLARE_PUSH_CONSTANTS_AT(typeName, variableName, registerIndex, registerSpace) \
    ConstantBuffer<typeName> variableName : \
        register(HE_PUSH_CONSTANT_REGISTER(registerIndex), registerSpace)
#endif

#define HE_DECLARE_PUSH_CONSTANTS(typeName, variableName, registerSpace) \
    HE_DECLARE_PUSH_CONSTANTS_AT(typeName, variableName, 0, registerSpace)

#endif
