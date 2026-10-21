#pragma htype cs CS

#include "Shared/BackendPushConstants.hlsli"
#include "Shared/SurfaceLightingMath.hlsli"
#include "RasterSurfaceData.hlsli"

struct SkinSubsurfaceConstants
{
    float4x4 InverseViewProjection;
    float4 CameraRadius; // camera world position, red-channel sigma in meters
    float4 PrimaryFog;  // start, base height, falloff, density
    float4 SecondaryFog;
    uint2 Dimensions;
    uint Operation; // 0 horizontal diffuse, 1 vertical and HDR composition
    uint Quality;
};
HE_DECLARE_PUSH_CONSTANTS(SkinSubsurfaceConstants, g_Skin, space3);
Texture2D<float4> g_InputDiffuse : register(t0, space3);
Texture2D<float4> g_OriginalDiffuse : register(t1, space3);
Texture2D<float4> g_Color : register(t2, space3);
Texture2D<float> g_Depth : register(t3, space3);
Texture2D<float4> g_Normal : register(t4, space3);
Texture2D<float4> g_Model : register(t5, space3);
Texture2D<uint2> g_SurfaceId : register(t6, space3);
RWTexture2D<float4> g_Output : register(u0, space3);

float3 PositionAt(int2 pixel, float depth)
{
    const float2 uv = (float2(pixel) + 0.5f) / float2(g_Skin.Dimensions);
    return HE_ReconstructRasterPosition(uv, depth, g_Skin.InverseViewProjection);
}

[numthreads(8, 8, 1)]
void CS(uint3 dispatchId : SV_DispatchThreadID)
{
    const int2 pixel = int2(dispatchId.xy);
    if (any(dispatchId.xy >= g_Skin.Dimensions)) return;
    const float4 originalDiffuse = g_OriginalDiffuse.Load(int3(pixel, 0));
    const float4 originalColor = g_Color.Load(int3(pixel, 0));
    if (originalDiffuse.a < 0.5f)
    {
        g_Output[pixel] = g_Skin.Operation == 0u ? 0.0f : originalColor;
        return;
    }

    const float depth = g_Depth.Load(int3(pixel, 0));
    const float3 position = PositionAt(pixel, depth);
    const int2 axis = g_Skin.Operation == 0u ? int2(1, 0) : int2(0, 1);
    const float metersPerPixel = length(PositionAt(pixel + axis, depth) - position);
    // Project a physical profile into the current view, rather than using a
    // fixed pixel blur which changes the apparent scale of a moving character.
    const float sigmaPixels = min(g_Skin.CameraRadius.w / max(metersPerPixel, 1e-8f), 32.0f);
    const uint objectId = g_SurfaceId.Load(int3(pixel, 0)).x;
    const float3 normal = HE_DecodeOctahedralNormal(g_Normal.Load(int3(pixel, 0)).xy);
    const float3 center = g_InputDiffuse.Load(int3(pixel, 0)).rgb;
    float3 filtered = center;
    if (sigmaPixels > 0.35f)
    {
        const int halfTaps = g_Skin.Quality >= 2u ? 6 : 3;
        float3 sum = 0.0f, totalWeight = 0.0f;
        [loop]
        for (int index = -halfTaps; index <= halfTaps; ++index)
        {
            const float offset = 3.0f * float(index) / float(halfTaps);
            const int2 samplePixel = pixel + axis * int(round(offset * sigmaPixels));
            // Wavelength-dependent Gaussian profile; red travels farther than
            // green and blue. Each channel is normalized independently.
            const float3 profileOffset = offset / float3(1.0f, 0.48f, 0.25f);
            const float3 weight = exp(-0.5f * profileOffset * profileOffset);
            float3 sampleColor = center;
            if (all(samplePixel >= 0) && all(samplePixel < int2(g_Skin.Dimensions)))
            {
                const int3 sampleLocation = int3(samplePixel, 0);
                const float4 sampleDiffuse = g_OriginalDiffuse.Load(sampleLocation);
                if (sampleDiffuse.a > 0.5f && g_SurfaceId.Load(sampleLocation).x == objectId)
                {
                    const float3 samplePosition = PositionAt(samplePixel, g_Depth.Load(sampleLocation));
                    const float3 sampleNormal = HE_DecodeOctahedralNormal(g_Normal.Load(sampleLocation).xy);
                    // Test displacement along the surface normal to reject
                    // overlapping lips/eyelids and back surfaces, not slopes.
                    const float planeDistance = abs(dot(samplePosition - position, normal));
                    const float edgeWeight = saturate(1.0f - planeDistance /
                        max(g_Skin.CameraRadius.w * 2.0f, metersPerPixel));
                    const float normalWeight = smoothstep(0.25f, 0.75f, dot(normal, sampleNormal));
                    sampleColor = lerp(center, g_InputDiffuse.Load(sampleLocation).rgb,
                        edgeWeight * normalWeight);
                }
            }
            // Rejected samples use the center, conserving energy at silhouettes.
            sum += sampleColor * weight;
            totalWeight += weight;
        }
        filtered = sum / max(totalWeight, 1e-6f);
    }

    if (g_Skin.Operation == 0u)
        g_Output[pixel] = float4(filtered, 1.0f);
    else
    {
        const float distance = length(position - g_Skin.CameraRadius.xyz);
        const float opticalDepth = HE_HeightFogOpticalDepth(g_Skin.CameraRadius.y, position.y,
            distance, g_Skin.PrimaryFog.x, g_Skin.PrimaryFog.y, g_Skin.PrimaryFog.z, g_Skin.PrimaryFog.w)
            + HE_HeightFogOpticalDepth(g_Skin.CameraRadius.y, position.y, distance,
                g_Skin.SecondaryFog.x, g_Skin.SecondaryFog.y, g_Skin.SecondaryFog.z, g_Skin.SecondaryFog.w);
        const float3 strength = saturate(-g_Model.Load(int3(pixel, 0)).rgb);
        const float3 result = originalColor.rgb +
            (filtered - originalDiffuse.rgb) * strength * exp(-opticalDepth);
        g_Output[pixel] = float4(clamp(result, 0.0f, 65504.0f), originalColor.a);
    }
}
