#pragma htype cs CS
#include "Shared/BackendPushConstants.hlsli"
struct Constants { uint2 Dimensions; uint2 Padding; };
HE_DECLARE_PUSH_CONSTANTS(Constants, g_Constants, space3);
RWTexture2D<float4> g_Color : register(u0, space3);
[numthreads(8, 8, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_Constants.Dimensions)) return;
    // RGB stays scene-linear. From here until FSR, alpha stores transparent
    // coverage: mesh alpha blending accumulates it, VFX blend/add takes its max.
    g_Color[id.xy] = float4(g_Color[id.xy].rgb, 0.0f);
}
