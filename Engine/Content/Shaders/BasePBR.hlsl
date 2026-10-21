#include "Include/Common.hlsli"
#include "Include/Skinning.hlsli"
#include "Include/PBR/BRDF.hlsli"
#include "Include/PBR/IBL.hlsli"

#pragma htype vs VS
#pragma htype ps PS
#pragma hvariant SKINNED

namespace Properties
{
#pragma property g_AlbedoMap             "Base Color Map"     texture  default(white)   group("Surface")
#pragma property g_BaseColorFactor       "Base Color"         color    default(1,1,1,1) group("Surface")
#pragma property g_MetallicRoughnessMap  "Metallic Roughness" texture  default(white)   group("Surface")
#pragma property g_MetallicMap           "Metallic Override"  texture  default(white)   group("Surface")
#pragma property g_RoughnessMap          "Roughness Override" texture  default(white)   group("Surface")
#pragma property g_MetallicFactor        "Metallic"           float    default(1.0)     range(0.0, 1.0)  group("Surface")
#pragma property g_RoughnessFactor       "Roughness"          float    default(1.0)     range(0.04, 1.0) group("Surface")
#pragma property g_AoMap                 "Ambient Occlusion"  texture  default(white)   group("Surface")
#pragma property g_AoStrength            "AO Strength"        float    default(1.0)     range(0.0, 1.0)  group("Surface")

#pragma property g_NormalMap             "Normal Map"         texture  default(normal)  group("Normals")
#pragma property g_NormalStrength        "Normal Strength"    float    default(1.0)     range(0.0, 2.0)  group("Normals")

#pragma property g_EmissiveMap           "Emissive Map"       texture  default(black)   group("Emission")
#pragma property g_EmissiveFactor        "Emissive (cd/m2)"   color    default(0,0,0,1) group("Emission")

#pragma property g_AlphaClip             "Alpha Clip"         float    default(0.0)     range(0.0, 1.0)  group("Advanced")
#pragma property g_AlphaCutoff           "Alpha Cutoff"       float    default(0.5)     range(0.0, 1.0)  group("Advanced")

    Texture2D g_AlbedoMap            : register(t0, space1);
    Texture2D g_NormalMap            : register(t1, space1);
    Texture2D g_MetallicRoughnessMap : register(t2, space1);
    Texture2D g_AoMap                : register(t3, space1);
    Texture2D g_EmissiveMap          : register(t7, space1);
    Texture2D g_MetallicMap          : register(t8, space1);
    Texture2D g_RoughnessMap         : register(t9, space1);

    cbuffer MaterialSurface : register(b3, space1)
    {
        float4 g_BaseColorFactor;
        float4 g_EmissiveFactor;
        float g_MetallicFactor;
        float g_RoughnessFactor;
        float g_AoStrength;
        float g_NormalStrength;
        float g_AlphaClip;
        float g_AlphaCutoff;
        float2 _MaterialPadding;
    }
}

float ComputePointLightAttenuation(float distanceToLight, float radius)
{
    const float safeRadius = max(radius, 1e-3f);
    const float safeDistance = max(distanceToLight, 1e-2f);
    const float normalizedDistance = distanceToLight / safeRadius;
    const float falloff = saturate(1.0f - normalizedDistance * normalizedDistance * normalizedDistance * normalizedDistance);
    return (falloff * falloff) / (safeDistance * safeDistance);
}

int SelectDirectionalShadowCascade(float3 positionWS)
{
    const int cascadeCount = min(max((int)g_DirectionalShadowSettings.y, 1), 4);
    const float viewDepth = abs(mul(g_ShadowCameraView, float4(positionWS, 1.0f)).z);

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        if (i >= cascadeCount)
            break;

        if (viewDepth <= g_DirectionalShadowCascadeSplits[i])
            return i;
    }

    return -1;
}

float SampleDirectionalShadow(float2 uv, int cascadeIndex, float compareDepth)
{
    const float shadowMapSize = max(g_DirectionalShadowSettings.z, 1.0f);
    const float texelSize = 1.0f / shadowMapSize;
    const float pcfRadius = max(g_DirectionalShadowBias.z, 0.0f);

    float visibility = 0.0f;
    if (g_DirectionalShadowBias.w < 0.5f || pcfRadius <= 0.0f)
    {
        visibility = g_DirectionalShadowMap.SampleCmpLevelZero(g_ShadowComparisonSampler, float3(uv, (float)cascadeIndex), compareDepth);
    }
    else
    {
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            [unroll]
            for (int x = -1; x <= 1; ++x)
            {
                const float2 offset = float2((float)x, (float)y) * texelSize * pcfRadius;
                visibility += g_DirectionalShadowMap.SampleCmpLevelZero(g_ShadowComparisonSampler, float3(uv + offset, (float)cascadeIndex), compareDepth);
            }
        }
        visibility /= 9.0f;
    }

    return visibility;
}

float SampleDirectionalShadowCascadeVisibility(
    float3 positionWS,
    float3 normalWS,
    float3 lightWS,
    int cascadeIndex)
{
    const float normalBiasScale = saturate(1.0f - dot(normalWS, lightWS));
    const float3 biasedPositionWS = positionWS + normalWS * g_DirectionalShadowBias.y * normalBiasScale;
    const float4 shadowPosition = mul(g_DirectionalShadowLightViewProjection[cascadeIndex], float4(biasedPositionWS, 1.0f));
    if (shadowPosition.w <= 0.0f)
        return 1.0f;

    const float3 projected = shadowPosition.xyz / shadowPosition.w;
    const float2 uv = float2(projected.x * 0.5f + 0.5f, -projected.y * 0.5f + 0.5f);
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f || projected.z < 0.0f || projected.z > 1.0f)
        return 1.0f;

    const float compareDepth = saturate(projected.z - g_DirectionalShadowBias.x);
    const float visibility = SampleDirectionalShadow(uv, cascadeIndex, compareDepth);
    return lerp(1.0f, visibility, saturate(g_DirectionalShadowSettings.w));
}

float ComputeDirectionalShadowVisibility(float3 positionWS, float3 normalWS, float3 lightWS)
{
    if (g_DirectionalShadowSettings.x < 0.5f)
        return 1.0f;

    const int cascadeIndex = SelectDirectionalShadowCascade(positionWS);
    if (cascadeIndex < 0)
        return 1.0f;

    const int cascadeCount = min(max((int)g_DirectionalShadowSettings.y, 1), 4);
    const float viewDepth = abs(mul(g_ShadowCameraView, float4(positionWS, 1.0f)).z);
    float visibility = SampleDirectionalShadowCascadeVisibility(
        positionWS,
        normalWS,
        lightWS,
        cascadeIndex);

    const float previousSplit = cascadeIndex > 0
        ? g_DirectionalShadowCascadeSplits[cascadeIndex - 1]
        : 0.0f;
    const float cascadeSpan = max(
        g_DirectionalShadowCascadeSplits[cascadeIndex] - previousSplit,
        1e-3f);

    if (cascadeIndex + 1 < cascadeCount)
    {
        const float blendRange = cascadeSpan * saturate(g_DirectionalShadowMisc.y);
        const float blendStart = g_DirectionalShadowCascadeSplits[cascadeIndex] - blendRange;
        if (blendRange > 0.0f && viewDepth > blendStart)
        {
            const float nextVisibility = SampleDirectionalShadowCascadeVisibility(
                positionWS,
                normalWS,
                lightWS,
                cascadeIndex + 1);
            const float blend = smoothstep(
                blendStart,
                g_DirectionalShadowCascadeSplits[cascadeIndex],
                viewDepth);
            visibility = lerp(visibility, nextVisibility, blend);
        }
    }
    else
    {
        const float fadeRange = cascadeSpan * saturate(g_DirectionalShadowMisc.z);
        const float fadeStart = g_DirectionalShadowCascadeSplits[cascadeIndex] - fadeRange;
        if (fadeRange > 0.0f && viewDepth > fadeStart)
        {
            const float fade = smoothstep(
                fadeStart,
                g_DirectionalShadowCascadeSplits[cascadeIndex],
                viewDepth);
            visibility = lerp(visibility, 1.0f, fade);
        }
    }

    return visibility;
}

#ifdef SKINNED
SurfaceVaryings VS(VSInputSkinned input, uint instanceId : SV_InstanceID)
{
    const float4x4 skinMatrix = ComputeSkinMatrix(input.BoneIDs, input.Weights, instanceId);
    const float3 skinnedPos = mul(skinMatrix, float4(input.pos, 1.0f)).xyz;
    const float3 skinnedNormal = mul((float3x3)skinMatrix, input.normal);
    const float3 skinnedTangent = mul((float3x3)skinMatrix, input.tangent);
    const float3 skinnedBitangent = mul((float3x3)skinMatrix, input.bitangent);
    return BuildSurfaceVaryings(skinnedPos, skinnedNormal, skinnedTangent, skinnedBitangent, input.texcoord, instanceId);
}
#else
SurfaceVaryings VS(VertexIn input, uint instanceId : SV_InstanceID)
{
    return BuildSurfaceVaryings(input.pos, input.normal, input.tangent, input.bitangent, input.texcoord, instanceId);
}
#endif

float4 PS(SurfaceVaryings input, bool bIsFrontFace : SV_IsFrontFace) : SV_Target
{
    const float faceSign = bIsFrontFace ? 1.0f : -1.0f;
    const float3 geometricNormalWS = SafeNormalize(input.NormalWS) * faceSign;
    const float4 albedoSample = Properties::g_AlbedoMap.Sample(g_LinearWrapSampler, input.TexCoord);
    const float alpha = albedoSample.a * g_BaseColorFactor.a;
    if (g_AlphaClip >= 0.5f)
        clip(alpha - g_AlphaCutoff);

    PbrSurfaceData surface;
    surface.BaseColor = SRGBToLinear(albedoSample.rgb) * g_BaseColorFactor.rgb;

    const float4 metallicRoughnessSample = Properties::g_MetallicRoughnessMap.Sample(g_LinearWrapSampler, input.TexCoord);
    const float metallicSample = metallicRoughnessSample.b;
    const float roughnessSample = metallicRoughnessSample.g;
    const float metallicOverride = Properties::g_MetallicMap.Sample(g_LinearWrapSampler, input.TexCoord).r;
    const float roughnessOverride = Properties::g_RoughnessMap.Sample(g_LinearWrapSampler, input.TexCoord).r;
    surface.Metallic = saturate(metallicSample * metallicOverride * g_MetallicFactor);
    surface.Roughness = clamp(roughnessSample * roughnessOverride * g_RoughnessFactor, 0.04f, 1.0f);

    const float aoSample = Properties::g_AoMap.Sample(g_LinearWrapSampler, input.TexCoord).r;
    surface.AmbientOcclusion = lerp(1.0f, aoSample, saturate(g_AoStrength));
    surface.Emissive = SRGBToLinear(Properties::g_EmissiveMap.Sample(g_LinearWrapSampler, input.TexCoord).rgb) * g_EmissiveFactor.rgb;
    surface.Alpha = alpha;

    float normalMipVariance;
    const float3 normalWS = SampleNormalTexture(
        Properties::g_NormalMap,
        input.TexCoord,
        geometricNormalWS,
        input.TangentWS,
        input.BitangentWS * faceSign,
        g_NormalStrength, normalMipVariance);
    surface.Roughness = ApplySpecularAntiAliasing(surface.Roughness, normalWS, normalMipVariance);

    const float3 viewWS = SafeNormalize(g_CameraPos - input.PositionWS);
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), surface.BaseColor, surface.Metallic);

    float3 directLighting = float3(0.0f, 0.0f, 0.0f);
    [unroll]
    for (int i = 0; i < 5; ++i)
    {
        if (g_DirLight[i].intensity > 0.0f)
        {
            const float3 lightWS = SafeNormalize(-g_DirLight[i].direction);
            const float3 radiance = g_DirLight[i].color * g_DirLight[i].intensity;
            // Shadow receiver bias must use the geometric normal. A high-frequency
            // normal map makes the receiver position oscillate and causes acne.
            const float shadowVisibility = i == (int)g_DirectionalShadowMisc.x
                ? ComputeDirectionalShadowVisibility(input.PositionWS, geometricNormalWS, lightWS)
                : 1.0f;
            directLighting += EvaluateDirectBRDF(surface, normalWS, viewWS, lightWS, radiance, F0) * shadowVisibility;
        }

        if (g_PointLight[i].intensity > 0.0f && g_PointLight[i].radius > 0.0f)
        {
            const float3 toLight = g_PointLight[i].position - input.PositionWS;
            const float distanceToLight = length(toLight);
            if (distanceToLight < g_PointLight[i].radius)
            {
                const float3 lightWS = toLight / max(distanceToLight, 1e-4f);
                const float attenuation = ComputePointLightAttenuation(
                    distanceToLight,
                    g_PointLight[i].radius);
                const float3 radiance = g_PointLight[i].color * g_PointLight[i].intensity * attenuation;
                directLighting += EvaluateDirectBRDF(surface, normalWS, viewWS, lightWS, radiance, F0);
            }
        }
    }

    float3 color = directLighting;
    color += EvaluateImageBasedLighting(surface, normalWS, geometricNormalWS, viewWS, F0);
    color += surface.Emissive;

    // Material passes always write linear HDR. Exposure, ACES and the output
    // transfer function are applied once by SceneToneMap.
    return float4(color, surface.Alpha);
}
