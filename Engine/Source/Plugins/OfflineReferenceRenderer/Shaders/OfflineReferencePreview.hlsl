#pragma htype cs CS
#pragma hvariant FLOAT4
#pragma hvariant SCALAR
#pragma hvariant UINT
#pragma hvariant TONEMAP

#include "Shared/BackendPushConstants.hlsli"

struct OfflineReferencePreviewConstants
{
    uint2 Dimensions;
    uint Aov;
    uint TargetSamples;
};
HE_DECLARE_PUSH_CONSTANTS(
    OfflineReferencePreviewConstants,
    g_PreviewConstants,
    space3);

#if defined(SCALAR)
Texture2D<float> g_Source : register(t0, space3);
#elif defined(UINT)
Texture2D<uint> g_Source : register(t0, space3);
#else
Texture2D<float4> g_Source : register(t0, space3);
#endif
RWTexture2D<float4> g_Output : register(u0, space3);

static const uint c_AovAlbedo = 3u;
static const uint c_AovWorldNormal = 4u;
static const uint c_AovViewDepth = 5u;
static const uint c_AovSampleCount = 8u;
static const uint c_InvalidIndex = 0xffffffffu;
static const float c_MaxFiniteHalf = 65504.0f;

float3 SanitizeHdr(float3 color)
{
    if (!all(isfinite(color)))
        return 0.0f;
    return clamp(color, 0.0f, c_MaxFiniteHalf);
}

float3 ToneMapACES(float3 color)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((color * (a * color + b)) /
        (color * (c * color + d) + e));
}

float3 LinearToSRGB(float3 color)
{
    const float3 lower = color * 12.92f;
    const float3 upper = 1.055f * pow(max(color, 0.0f), 1.0f / 2.4f) -
        0.055f;
    return lerp(upper, lower, color <= 0.0031308f);
}

uint HashId(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

float3 VisualizeId(uint value)
{
    if (value == c_InvalidIndex)
        return 0.0f;
    const uint hash = HashId(value + 1u);
    return 0.2f + 0.8f * float3(
        float((hash >> 0u) & 0xffu),
        float((hash >> 8u) & 0xffu),
        float((hash >> 16u) & 0xffu)) / 255.0f;
}

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= g_PreviewConstants.Dimensions))
        return;

#if defined(TONEMAP)
    const float3 sceneLinear = SanitizeHdr(g_Source.Load(int3(pixel, 0)).rgb);
    g_Output[pixel] = float4(LinearToSRGB(ToneMapACES(sceneLinear)), 1.0f);
#elif defined(SCALAR)
    const float value = g_Source.Load(int3(pixel, 0));
    const float mapped = g_PreviewConstants.Aov == c_AovViewDepth &&
            isfinite(value) && value > 0.0f
        ? value / (1.0f + value)
        : 0.0f;
    g_Output[pixel] = float4(mapped.xxx, 1.0f);
#elif defined(UINT)
    const uint value = g_Source.Load(int3(pixel, 0));
    if (g_PreviewConstants.Aov == c_AovSampleCount)
    {
        const float mapped = saturate(float(value) /
            float(max(g_PreviewConstants.TargetSamples, 1u)));
        g_Output[pixel] = float4(mapped.xxx, 1.0f);
    }
    else
    {
        g_Output[pixel] = float4(VisualizeId(value), 1.0f);
    }
#else
    const float4 source = g_Source.Load(int3(pixel, 0));
    float3 color = SanitizeHdr(source.rgb);
    if (g_PreviewConstants.Aov == c_AovAlbedo)
        color = saturate(color);
    else if (g_PreviewConstants.Aov == c_AovWorldNormal)
        color = saturate(source.rgb * 0.5f + 0.5f);
    g_Output[pixel] = float4(color, 1.0f);
#endif
}
