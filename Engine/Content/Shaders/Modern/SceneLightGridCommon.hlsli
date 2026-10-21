#ifndef HE_SCENE_LIGHT_GRID_COMMON_HLSLI
#define HE_SCENE_LIGHT_GRID_COMMON_HLSLI

struct HESceneLight
{
    float4 PositionRadius;
    float4 ColorIntensity;
    float4 DirectionOuterCosine;
    // x: inner cosine, y: reserved, z: 0 point/1 spot/2 sun,
    // w: directional CSM ownership, or local shadow base layer + 1 (zero=none).
    float4 ConeReservedTypeShadow;
};

cbuffer SceneLightGridConstants : register(b1, space0)
{
    float4x4 g_LightGridView;
    float4x4 g_LightGridInverseProjection;
    float4x4 g_LightGridViewProjection;
    uint4 g_LightGridDimensions;
    // width, height, light count, tile edge in pixels.
    uint4 g_LightGridFrameDimensions;
    // near, far, inverse depth span (log2 for perspective), signed slice count.
    // A negative slice count selects linear orthographic depth, including a
    // signed near plane; perspective uses positive logarithmic slices.
    float4 g_LightGridDepthParameters;
};

StructuredBuffer<HESceneLight> g_SceneLights : register(t0, space0);
#ifndef HE_BUILD_LIGHT_GRID
StructuredBuffer<uint> g_ClusterLightMasks : register(t1, space0);

uint HE_GetSceneLightCluster(float3 positionWS)
{
    const float4 clip = mul(g_LightGridViewProjection, float4(positionWS, 1.0f));
    const float2 uv = clip.xy / max(abs(clip.w), 1e-8f) *
        float2(0.5f, -0.5f) + 0.5f;
    const uint2 tile = min(uint2(max(uv, 0.0f) *
        float2(g_LightGridFrameDimensions.xy)) / g_LightGridFrameDimensions.w,
        g_LightGridDimensions.xy - 1u);
    const float viewZ = mul(g_LightGridView, float4(positionWS, 1.0f)).z;
    const bool bOrthographic = g_LightGridDepthParameters.w < 0.0f;
    const float viewDepth = bOrthographic
        ? viewZ * (g_LightGridInverseProjection[2][2] < 0.0f ? -1.0f : 1.0f)
        : abs(viewZ);
    const float normalizedDepth = bOrthographic
        ? (viewDepth - g_LightGridDepthParameters.x) * g_LightGridDepthParameters.z
        : log2(max(viewDepth, g_LightGridDepthParameters.x) /
            g_LightGridDepthParameters.x) * g_LightGridDepthParameters.z;
    const uint slice = min(uint(max(normalizedDepth, 0.0f) *
        abs(g_LightGridDepthParameters.w)), g_LightGridDimensions.z - 1u);
    return (slice * g_LightGridDimensions.y + tile.y) * g_LightGridDimensions.x + tile.x;
}
#endif
#endif
