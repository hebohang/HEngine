#pragma htype cs CS
#include "Shared/BackendPushConstants.hlsli"
struct Constants { uint2 Dimensions; uint2 Padding; float4x4 SkyReprojection; };
HE_DECLARE_PUSH_CONSTANTS(Constants, g_Constants, space3);
Texture2D<float4> g_Color : register(t0, space3);
Texture2D<float4> g_Opaque : register(t1, space3);
Texture2D<float> g_Depth : register(t2, space3);
Texture2D<float4> g_Motion : register(t3, space3);
RWTexture2D<float> g_DepthCopy : register(u0, space3);
RWTexture2D<float> g_Reactive : register(u1, space3);
RWTexture2D<float> g_Composition : register(u2, space3);
RWTexture2D<float2> g_MotionCopy : register(u3, space3);
[numthreads(8, 8, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_Constants.Dimensions)) return;
    float3 color = max(0.0f, g_Color[id.xy].rgb);
    float3 opaque = max(0.0f, g_Opaque[id.xy].rgb);
    // Relative scene-linear differences catch bright emissive particles too;
    // the denominator prevents scene exposure units from changing the mask.
    float3 difference = abs(color - opaque) / max(max(color, opaque), 0.01f);
    float reactive = saturate(max(difference.x, max(difference.y, difference.z)) * 0.9f);
    // Coverage catches low-contrast alpha surfaces that a color difference misses.
    // Keep the difference path for distortion and subtract/multiply VFX, whose
    // blending preserves destination alpha. The SDK recommends capping reactive
    // below one; composition independently retains the full coverage.
    float coverage = saturate(g_Color[id.xy].a);
    reactive = max(reactive, coverage * 0.9f);
    float depth = g_Depth[id.xy];
    float2 motion = g_Motion[id.xy].xy;
    // Scene raster uses conventional depth with a clear value of exactly one.
    // Reproject the infinite sky using camera rotation and both frames' jitter.
    if (depth == 1.0f)
    {
        float2 uv = (float2(id.xy) + 0.5f) / float2(g_Constants.Dimensions);
        float4 previous = mul(g_Constants.SkyReprojection, float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 1.0f, 1.0f));
        motion = previous.w > 1e-5f
            ? previous.xy / previous.w * float2(0.5f, -0.5f) + 0.5f - uv
            : float2(2.0f, 2.0f); // Behind the previous view: reject history.
    }
    g_MotionCopy[id.xy] = motion;
    g_DepthCopy[id.xy] = depth;
    g_Reactive[id.xy] = reactive;
    g_Composition[id.xy] = max(reactive, coverage);
}
