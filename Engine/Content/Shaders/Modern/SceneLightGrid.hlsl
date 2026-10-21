#pragma htype cs CS

#define HE_BUILD_LIGHT_GRID 1
#include "SceneLightGridCommon.hlsli"

RWStructuredBuffer<uint> g_ClusterLightMasks : register(u0, space0);

void AccumulateDepthCorner(float2 uv, float nearDepth, float farDepth,
    inout float3 boundsMin, inout float3 boundsMax)
{
    const float2 xy = uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    const float4 nearH = mul(g_LightGridInverseProjection, float4(xy, 0.0f, 1.0f));
    const float4 farH = mul(g_LightGridInverseProjection, float4(xy, 1.0f, 1.0f));
    const float3 nearPoint = nearH.xyz / nearH.w;
    const float3 farPoint = farH.xyz / farH.w;
    const float3 delta = farPoint - nearPoint;
    const float directionSign = farPoint.z < 0.0f ? -1.0f : 1.0f;
    const float3 first = nearPoint + delta *
        ((nearDepth * directionSign - nearPoint.z) / delta.z);
    const float3 last = nearPoint + delta *
        ((farDepth * directionSign - nearPoint.z) / delta.z);
    boundsMin = min(boundsMin, min(first, last));
    boundsMax = max(boundsMax, max(first, last));
}

[numthreads(64, 1, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint clusterCount = g_LightGridDimensions.x *
        g_LightGridDimensions.y * g_LightGridDimensions.z;
    const uint cluster = dispatchThreadId.x;
    if (cluster >= clusterCount)
        return;
    const uint slice = cluster / (g_LightGridDimensions.x * g_LightGridDimensions.y);
    const uint tileY = (cluster / g_LightGridDimensions.x) % g_LightGridDimensions.y;
    const uint tileX = cluster % g_LightGridDimensions.x;
    const bool bOrthographic = g_LightGridDepthParameters.w < 0.0f;
    const float sliceCount = abs(g_LightGridDepthParameters.w);
    const float ratio = g_LightGridDepthParameters.y / max(g_LightGridDepthParameters.x, 1e-5f);
    const float nearDepth = bOrthographic
        ? lerp(g_LightGridDepthParameters.x, g_LightGridDepthParameters.y, float(slice) / sliceCount)
        : g_LightGridDepthParameters.x * pow(ratio, float(slice) / sliceCount);
    const float farDepth = bOrthographic
        ? lerp(g_LightGridDepthParameters.x, g_LightGridDepthParameters.y, float(slice + 1u) / sliceCount)
        : g_LightGridDepthParameters.x * pow(ratio, float(slice + 1u) / sliceCount);
    // Two pixels conservatively cover the raster jitter footprint, projection
    // roundoff and fragments on tile boundaries; no opaque depth is consulted.
    const float2 uvMin = (float2(tileX, tileY) * g_LightGridFrameDimensions.w - 2.0f) /
        float2(g_LightGridFrameDimensions.xy);
    const float2 uvMax = (float2(tileX + 1u, tileY + 1u) *
        g_LightGridFrameDimensions.w + 2.0f) / float2(g_LightGridFrameDimensions.xy);
    float3 boundsMin = 3.402823466e+38f;
    float3 boundsMax = -3.402823466e+38f;
    AccumulateDepthCorner(uvMin, nearDepth, farDepth, boundsMin, boundsMax);
    AccumulateDepthCorner(float2(uvMin.x, uvMax.y), nearDepth, farDepth, boundsMin, boundsMax);
    AccumulateDepthCorner(float2(uvMax.x, uvMin.y), nearDepth, farDepth, boundsMin, boundsMax);
    AccumulateDepthCorner(uvMax, nearDepth, farDepth, boundsMin, boundsMax);
    [loop]
    for (uint word = 0; word < g_LightGridDimensions.w; ++word)
    {
        uint mask = 0u;
        [loop]
        for (uint bit = 0; bit < 32u; ++bit)
        {
            const uint lightIndex = word * 32u + bit;
            if (lightIndex >= g_LightGridFrameDimensions.z)
                break;
            const HESceneLight light = g_SceneLights[lightIndex];
            const float3 center = mul(g_LightGridView,
                float4(light.PositionRadius.xyz, 1.0f)).xyz;
            const float3 difference = max(max(boundsMin - center, center - boundsMax), 0.0f);
            // A spotlight's range sphere encloses its entire cone. Using it
            // is intentionally conservative for both opaque and transparent.
            const bool bIntersects = light.ConeReservedTypeShadow.z > 1.5f ||
                dot(difference, difference) <= light.PositionRadius.w * light.PositionRadius.w;
            if (bIntersects)
                mask |= 1u << bit;
        }
        g_ClusterLightMasks[cluster * g_LightGridDimensions.w + word] = mask;
    }
}
