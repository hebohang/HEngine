#ifndef HE_SCENE_LOCAL_SHADOW_SAMPLING_HLSLI
#define HE_SCENE_LOCAL_SHADOW_SAMPLING_HLSLI

#include "ShadowContinuousPcf.hlsli"

struct HELocalShadowViewRecord
{
    float4x4 ViewProjection;
    float4 PositionRange;
    // inverse resolution, device depth bias, world normal bias, valid flag.
    float4 BiasSettings;
};

#ifndef HE_LOCAL_SHADOW_DEPTH_REGISTER
#define HE_LOCAL_SHADOW_DEPTH_REGISTER t10
#endif
#ifndef HE_LOCAL_SHADOW_VIEWS_REGISTER
#define HE_LOCAL_SHADOW_VIEWS_REGISTER t11
#endif
Texture2DArray<float> g_LocalShadowDepth : register(HE_LOCAL_SHADOW_DEPTH_REGISTER, space3);
StructuredBuffer<HELocalShadowViewRecord> g_LocalShadowViews : register(HE_LOCAL_SHADOW_VIEWS_REGISTER, space3);

uint HE_SelectPointShadowFace(float3 direction)
{
    const float3 magnitude = abs(direction);
    if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z)
        return direction.x >= 0.0f ? 0u : 1u;
    if (magnitude.y >= magnitude.z)
        return direction.y >= 0.0f ? 2u : 3u;
    return direction.z >= 0.0f ? 4u : 5u;
}

float HE_EvaluateLocalShadow(float3 positionWS, float3 geometricNormalWS,
    float3 lightPosition, bool bPointLight, uint firstView)
{
    const uint viewIndex = firstView + (bPointLight
        ? HE_SelectPointShadowFace(positionWS - lightPosition) : 0u);
    const HELocalShadowViewRecord view = g_LocalShadowViews[viewIndex];
    if (view.BiasSettings.w < 0.5f)
        return 1.0f;
    const float3 biasedPosition = positionWS + geometricNormalWS * view.BiasSettings.z;
    const float4 clip = mul(view.ViewProjection, float4(biasedPosition, 1.0f));
    if (clip.w <= 0.0f)
        return 1.0f;
    const float3 ndc = clip.xyz / clip.w;
    const float2 uv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
    if (any(uv < 0.0f) || any(uv > 1.0f) || ndc.z <= 0.0f || ndc.z >= 1.0f)
        return 1.0f;
    uint width, height, layers;
    g_LocalShadowDepth.GetDimensions(width, height, layers);
    if (g_ShadowSettings.w < 0.0f)
        return HE_ContinuousShadowPcf(g_LocalShadowDepth, uv, viewIndex,
            uint2(width, height), ndc.z - view.BiasSettings.y, 1.0f);
    const int2 pixel = int2(uv * float2(width, height));
    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const int2 samplePixel = clamp(pixel + int2(x, y), int2(0, 0),
                int2(width, height) - 1);
            const float shadowDepth = g_LocalShadowDepth.Load(int4(samplePixel, viewIndex, 0));
            visibility += ndc.z - view.BiasSettings.y <= shadowDepth ? 1.0f : 0.0f;
        }
    }
    return visibility / 9.0f;
}
#endif
