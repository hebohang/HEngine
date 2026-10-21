#pragma htype ps PS
#pragma hbackend DX12
#pragma hvariant GPU_DRIVEN
#pragma hvariant SKINNED
#pragma hvariant GBUFFER
#pragma hvariant GBUFFER_SKINNED
#pragma hvariant GBUFFER_GPU_DRIVEN
#pragma hvariant GUIDES
#pragma hvariant GUIDES_SKINNED
#pragma hvariant GUIDES_GPU_DRIVEN
#pragma hprofile ps ps_6_6

#define HE_HEAP_DIRECT 1
#include "SceneOpaque.hlsl"
