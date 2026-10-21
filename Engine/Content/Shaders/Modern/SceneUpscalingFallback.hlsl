#pragma htype cs CS
#include "Shared/BackendPushConstants.hlsli"
struct Constants { uint2 Dimensions; uint2 Padding; };
HE_DECLARE_PUSH_CONSTANTS(Constants, g_Constants, space3);
Texture2D<float4> g_Color : register(t0, space3);
RWTexture2D<float4> g_Output : register(u0, space3);
SamplerState g_LinearClamp : register(s0, space3);
[numthreads(8, 8, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_Constants.Dimensions)) return;
    g_Output[id.xy] = g_Color.SampleLevel(g_LinearClamp,
        (float2(id.xy) + 0.5f) / g_Constants.Dimensions, 0);
}
