#ifndef HE_MATERIAL_DESCRIPTOR_ACCESS_HLSLI
#define HE_MATERIAL_DESCRIPTOR_ACCESS_HLSLI

// Baseline raster uses bounded descriptor tables that fit D3D12 resource
// binding tier 1 limits. Modern raster opts into SM 6.6 heap-direct indexing
// through a separate shader source/profile so the baseline cannot acquire an
// accidental SM 6.6 dependency.
#ifndef HE_HEAP_DIRECT
#define HE_HEAP_DIRECT 0
#endif

#if HE_HEAP_DIRECT
#define HE_MATERIAL_TEXTURE(index) \
    ResourceDescriptorHeap[NonUniformResourceIndex(index)]
#define HE_MATERIAL_SAMPLER(index) \
    SamplerDescriptorHeap[NonUniformResourceIndex(index)]
#else
// Tier 1 exposes 128 SRVs and 16 samplers per shader stage. Reserve eight SRV
// slots and one sampler slot for GPU-scene, shadow and environment layouts in
// the same pixel-stage root signature.
#define HE_BASELINE_TEXTURE_DESCRIPTOR_COUNT 120
#define HE_BASELINE_SAMPLER_DESCRIPTOR_COUNT 15

Texture2D<float4> g_HEMaterialTextures[HE_BASELINE_TEXTURE_DESCRIPTOR_COUNT]
    : register(t0, space1);
SamplerState g_HEMaterialSamplers[HE_BASELINE_SAMPLER_DESCRIPTOR_COUNT]
    : register(s0, space4);

#define HE_MATERIAL_TEXTURE(index) \
    g_HEMaterialTextures[NonUniformResourceIndex(index)]
#define HE_MATERIAL_SAMPLER(index) \
    g_HEMaterialSamplers[NonUniformResourceIndex(index)]
#endif

#endif
