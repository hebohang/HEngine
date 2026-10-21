#ifndef HE_RASTER_SURFACE_DATA_HLSLI
#define HE_RASTER_SURFACE_DATA_HLSLI

// Raster surface ABI: linear base color/metalness (RGBA8_UNORM), world-space
// octahedral normal/perceptual roughness/AO (RGBA8_UNORM), linear HDR emissive
// and shading model 0=Lit,1=Unlit (RGBA16_FLOAT). Negative model IDs
// -2=skin,-3=hair,-4=cloth store non-positive model payload in RGB, never emission.
// Static baked diffuse and its IBL replacement weight have their own
// RGBA16_FLOAT target, so foliage transmission does not overwrite either.
// PreviousUV-currentUV and
// previous device/view depth (RGBA16_FLOAT), exact instance/primitive identity
// (RG32_UINT). A SurfaceId.x of 0xffffffff is background; depth alone is not an
// identity test. Custom HairCards reserve the high bit of SurfaceId.y so TAA
// can recognize intra-object card changes while preserving the depth/normal
// tests. Other surfaces keep exact primitive identity. No world position or
// high precision normal target is stored.
struct HERasterSurfaceOutputs
{
#if HE_RASTER_GUIDES_ONLY
    float4 NormalRoughnessAo : SV_Target0;
    float4 MotionDepth : SV_Target1;
    uint2 SurfaceId : SV_Target2;
#else
    float4 BaseColorMetalness : SV_Target0;
    float4 NormalRoughnessAo : SV_Target1;
    float4 EmissiveModel : SV_Target2;
    float4 MotionDepth : SV_Target3;
    uint2 SurfaceId : SV_Target4;
    float4 StaticDiffuse : SV_Target5;
#endif
};

float2 HE_OctahedralSign(float2 value)
{
    return float2(value.x >= 0.0f ? 1.0f : -1.0f,
        value.y >= 0.0f ? 1.0f : -1.0f);
}

float2 HE_EncodeOctahedralNormal(float3 normalWS)
{
    normalWS /= max(abs(normalWS.x) + abs(normalWS.y) + abs(normalWS.z), 1e-8f);
    float2 encoded = normalWS.xy;
    if (normalWS.z < 0.0f)
        encoded = (1.0f - abs(encoded.yx)) * HE_OctahedralSign(encoded);
    return encoded * 0.5f + 0.5f;
}

float3 HE_DecodeOctahedralNormal(float2 encoded)
{
    const float2 projected = encoded * 2.0f - 1.0f;
    float3 normalWS = float3(projected, 1.0f - abs(projected.x) - abs(projected.y));
    if (normalWS.z < 0.0f)
        normalWS.xy = (1.0f - abs(normalWS.yx)) * HE_OctahedralSign(normalWS.xy);
    return normalize(normalWS);
}

float4 HE_EncodeMotionDepth(float4 currentClip, float4 previousClip, float previousViewDepth)
{
    if (currentClip.w <= 1e-6f || previousClip.w <= 1e-6f ||
        !all(isfinite(currentClip)) || !all(isfinite(previousClip)) ||
        !isfinite(previousViewDepth) || previousViewDepth <= 0.0f)
        return float4(0.0f, 0.0f, -1.0f, 0.0f);
    const float3 currentNdc = currentClip.xyz / currentClip.w;
    const float3 previousNdc = previousClip.xyz / previousClip.w;
    const float2 motion = (previousNdc.xy - currentNdc.xy) * float2(0.5f, -0.5f);
    return float4(clamp(motion, -65504.0f, 65504.0f),
        previousNdc.z, min(previousViewDepth, 65504.0f));
}

HERasterSurfaceOutputs HE_EncodeRasterSurface(
    float3 baseColor, float metallic, float3 normalWS, float roughness, float ao,
    float3 emissive, bool bUnlit, float4 currentClip, float4 previousClip,
    float previousViewDepth, uint objectId, uint primitiveId)
{
    HERasterSurfaceOutputs output;
#if !HE_RASTER_GUIDES_ONLY
    output.BaseColorMetalness = float4(saturate(baseColor), saturate(metallic));
#endif
    output.NormalRoughnessAo = float4(HE_EncodeOctahedralNormal(normalWS),
        saturate(roughness), saturate(ao));
#if !HE_RASTER_GUIDES_ONLY
    output.EmissiveModel = float4(clamp(emissive, 0.0f, 65504.0f), bUnlit ? 1.0f : 0.0f);
    output.StaticDiffuse = 0.0f;
#endif
    output.MotionDepth = HE_EncodeMotionDepth(currentClip, previousClip, previousViewDepth);
    output.SurfaceId = uint2(objectId, primitiveId);
    return output;
}

float3 HE_ReconstructRasterPosition(float2 uv, float depth, float4x4 inverseViewProjection)
{
    const float4 world = mul(inverseViewProjection,
        float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), depth, 1.0f));
    return world.xyz / world.w;
}
#endif
