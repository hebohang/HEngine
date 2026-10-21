#pragma htype vs DeferredVS
#pragma htype ps DeferredPS

#include "RasterSurfaceData.hlsli"
#include "BakedIrradianceTypes.hlsli"

// Identical frame prefix and environment bindings to the Forward material ABI.
cbuffer FrameConstants : register(b0, space0)
{
    float4x4 g_ViewProjection;
    float4x4 g_LightViewProjection;
    float4 g_CameraPositionAndExposure;
    float4 g_DirectionalDirectionAndIntensity;
    float4 g_DirectionalColor;
    float4 g_PointPositionAndIntensity;
    float4 g_PointColorAndRadius;
    float4 g_ShadowSettings;
    float4 g_IblSettings;
    float4x4 g_InverseViewProjection;
    float4x4 g_ShadowCameraView;
    float4x4 g_AdditionalLightViewProjections[3];
    float4 g_DirectionalShadowCascadeSplits;
    float4 g_DirectionalShadowCascadeSettings;
    float4 g_DirectionalShadowDebugSettings;
    float4 g_MaterialTimeAndPadding;
    float4x4 g_PreviousViewProjection;
    float4x4 g_PreviousView;
    float4 g_FogColorDensity;
    float4 g_FogDistanceHeight;
    float4 g_FogDirectionalColorIntensity;
    float4 g_FogDirectionalParams;
    float4 g_SecondaryFogColorDensity;
    float4 g_SecondaryFogDistanceHeight;
    uint4 g_BakedIrradianceCount;
    HEBakedIrradianceVolume g_BakedIrradianceVolumes[16];
};

Texture2DArray<float> g_DirectionalShadow : register(t0, space3);
TextureCube<float4> g_IrradianceMap : register(t1, space3);
TextureCube<float4> g_PrefilterMap : register(t2, space3);
Texture2D<float2> g_BrdfLut : register(t3, space3);
SamplerState g_ShadowSampler : register(s0, space3);

Texture2D<float> g_RasterDepth : register(t4, space3);
Texture2D<float4> g_RasterBaseColorMetalness : register(t5, space3);
Texture2D<float4> g_RasterNormalRoughnessAo : register(t6, space3);
Texture2D<float4> g_RasterEmissiveModel : register(t7, space3);
Texture2D<float4> g_RasterMotionDepth : register(t8, space3);
Texture2D<uint2> g_RasterSurfaceId : register(t9, space3);
Texture2D<float4> g_RasterStaticDiffuse : register(t10, space3);

#define HE_LOCAL_SHADOW_DEPTH_REGISTER t11
#define HE_LOCAL_SHADOW_VIEWS_REGISTER t12
#include "RasterSurfaceLighting.hlsli"

struct DeferredVaryings
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

DeferredVaryings DeferredVS(uint vertexId : SV_VertexID)
{
    DeferredVaryings output;
    output.UV = float2((vertexId << 1u) & 2u, vertexId & 2u);
    output.Position = float4(output.UV * float2(2.0f, -2.0f) +
        float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

#if HE_DEFERRED_SKIN_SSS
struct DeferredSkinOutput
{
    float4 Color : SV_Target0;
    float4 Diffuse : SV_Target1;
};
DeferredSkinOutput DeferredPS(DeferredVaryings input)
#else
float4 DeferredPS(DeferredVaryings input) : SV_Target0
#endif
{
    const int3 pixel = int3(int2(input.Position.xy), 0);
    const float depth = g_RasterDepth.Load(pixel);
    const uint2 surfaceId = g_RasterSurfaceId.Load(pixel);
    if (surfaceId.x == 0xffffffffu || !isfinite(depth) || depth >= 1.0f)
        discard;
    const float4 baseColorMetalness = g_RasterBaseColorMetalness.Load(pixel);
    const float4 normalRoughnessAo = g_RasterNormalRoughnessAo.Load(pixel);
    const float4 emissiveModel = g_RasterEmissiveModel.Load(pixel);
    const float4 staticDiffuse = g_RasterStaticDiffuse.Load(pixel);
    const float3 worldPosition = HE_ReconstructRasterPosition(
        input.UV, depth, g_InverseViewProjection);
    const float3 normalWS = HE_DecodeOctahedralNormal(normalRoughnessAo.xy);
    // A depth derivative provides the geometric normal used by shadow normal
    // bias, independently from the normal map stored in the GBuffer.
    float3 geometricNormal = SafeNormalize(cross(ddx(worldPosition), ddy(worldPosition)));
    geometricNormal *= dot(geometricNormal, normalWS) >= 0.0f ? 1.0f : -1.0f;
    const uint characterModel = emissiveModel.w < -1.5f ? (uint)floor(-emissiveModel.w + 0.001f) : 0u;
    const float specularLevel = characterModel == 2u || characterModel == 6u
        ? saturate((-emissiveModel.w - (float)characterModel) * 2.0f) : 0.5f;
    const float3 characterPayload = saturate(-emissiveModel.rgb);
    const float3 strand = characterModel == 3u
        ? HE_DecodeOctahedralNormal(characterPayload.xy) : float3(0, 1, 0);
    const bool bStaticLightmap = emissiveModel.w > 1.5f && emissiveModel.w < 2.5f;
    const float staticLightmapBlend = bStaticLightmap
        ? saturate((emissiveModel.w - 2.0f) * 4.0f)
        : characterModel == 7u ? saturate(staticDiffuse.a) : 0.0f;
    const bool bUnlit = emissiveModel.w > 0.5f && !bStaticLightmap;
#if HE_DEFERRED_SKIN_SSS
    float3 skinDiffuse = 0.0f;
#endif
    const float3 radiance = bUnlit
        ? baseColorMetalness.rgb + emissiveModel.rgb
        : ShadeSceneSurface(
#if HE_DEFERRED_SKIN_SSS
            skinDiffuse,
#endif
            worldPosition, baseColorMetalness.rgb,
            baseColorMetalness.a, normalRoughnessAo.z, normalRoughnessAo.w,
            normalWS, geometricNormal,
            max(emissiveModel.rgb, 0.0f) + max(staticDiffuse.rgb, 0.0f), true,
            characterModel, characterPayload, strand, characterPayload.z,
            specularLevel, staticLightmapBlend);
    const float4 color = float4(SanitizeHalfRadiance(bUnlit
        ? HE_ApplyDistanceHeightFog(radiance, worldPosition) : radiance), 1.0f);
#if HE_DEFERRED_SKIN_SSS
    DeferredSkinOutput output;
    output.Color = color;
    output.Diffuse = float4(SanitizeHalfRadiance(skinDiffuse), characterModel == 2u ? 1.0f : 0.0f);
    return output;
#else
    return color;
#endif
}
