#pragma htype cs CS

#include "RasterSurfaceData.hlsli"
#include "Shared/BackendPushConstants.hlsli"

struct RasterSurfaceUnpackConstants
{
    float4x4 ViewFromClip;
    float4x4 PreviousSkyClipFromCurrentClip;
};
HE_DECLARE_PUSH_CONSTANTS(RasterSurfaceUnpackConstants, g_UnpackConstants, space3);

Texture2D<float> g_RasterDepth : register(t0, space3);
Texture2D<float4> g_RasterBaseColorMetalness : register(t1, space3);
Texture2D<float4> g_RasterNormalRoughnessAo : register(t2, space3);
Texture2D<float4> g_RasterEmissiveModel : register(t3, space3);
Texture2D<float4> g_RasterMotionDepth : register(t4, space3);
Texture2D<uint2> g_RasterSurfaceId : register(t5, space3);
RWTexture2D<float4> g_NormalRoughness : register(u0, space3);
RWTexture2D<float2> g_MotionVectors : register(u3, space3);
RWTexture2D<float> g_PreviousDepth : register(u5, space3);
RWTexture2D<float2> g_ViewDepth : register(u6, space3);

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width, height;
    g_RasterDepth.GetDimensions(width, height);
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= uint2(width, height)))
        return;
    const int3 location = int3(pixel, 0);
    const uint2 surfaceId = g_RasterSurfaceId.Load(location);
    const float depth = g_RasterDepth.Load(location);
    const bool bValid = surfaceId.x != 0xffffffffu && isfinite(depth) && depth < 1.0f;
    const float4 normalRoughnessAo = g_RasterNormalRoughnessAo.Load(location);
    const float4 motionDepth = g_RasterMotionDepth.Load(location);
    const float2 uv = (float2(pixel) + 0.5f) / float2(width, height);
    const float2 ndc = uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    const float4 viewPosition = mul(g_UnpackConstants.ViewFromClip, float4(ndc, depth, 1.0f));
    const float currentViewDepth = abs(viewPosition.z / viewPosition.w);
    const float4 previousSkyClip = mul(g_UnpackConstants.PreviousSkyClipFromCurrentClip,
        float4(ndc, 1.0f, 1.0f));
    // Infinite background follows camera rotation and jitter, never translation.
    // Directions behind the previous camera reject history through out-of-range UV.
    const float2 previousSkyUv = all(isfinite(previousSkyClip)) && previousSkyClip.w > 1e-6f
        ? (previousSkyClip.xy / previousSkyClip.w) * float2(0.5f, -0.5f) + 0.5f
        : float2(2.0f, 2.0f);
    g_NormalRoughness[pixel] = bValid
        ? float4(HE_DecodeOctahedralNormal(normalRoughnessAo.xy), normalRoughnessAo.z)
        : float4(0.0f, 0.0f, 1.0f, 1.0f);
#if !HE_TAA_GUIDES_ONLY
    // Base/metal, emissive/model and stable identity alias the authoritative
    // GBuffer. GI consumers gate diffuse albedo by emissive.a for Unlit.
    g_PreviousDepth[pixel] = bValid ? motionDepth.z : 1.0f;
#endif
    g_MotionVectors[pixel] = bValid ? motionDepth.xy : clamp(previousSkyUv - uv, -65504.0f, 65504.0f);
    g_ViewDepth[pixel] = bValid ? float2(currentViewDepth, motionDepth.w) : 0.0f;
}
