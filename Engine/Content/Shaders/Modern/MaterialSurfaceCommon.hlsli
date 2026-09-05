#ifndef HE_MATERIAL_SURFACE_COMMON_HLSLI
#define HE_MATERIAL_SURFACE_COMMON_HLSLI

#include "MaterialDescriptorAccess.hlsli"
#include "Shared/BackendPushConstants.hlsli"

// Heap-direct resources need an explicit type when a generated property macro
// is used as the left side of `.Sample(...)` rather than assigned first.
#if HE_HEAP_DIRECT
#undef HE_MATERIAL_TEXTURE
#undef HE_MATERIAL_SAMPLER
Texture2D<float4> HE_GetHeapMaterialTexture(uint index)
{
    Texture2D<float4> textureResource =
        ResourceDescriptorHeap[NonUniformResourceIndex(index)];
    return textureResource;
}
SamplerState HE_GetHeapMaterialSampler(uint index)
{
    SamplerState samplerResource =
        SamplerDescriptorHeap[NonUniformResourceIndex(index)];
    return samplerResource;
}
#define HE_MATERIAL_TEXTURE(index) HE_GetHeapMaterialTexture(index)
#define HE_MATERIAL_SAMPLER(index) HE_GetHeapMaterialSampler(index)
#endif

#ifndef HE_MATERIAL_PASS_FORWARD
#define HE_MATERIAL_PASS_FORWARD 1
#endif
#ifndef HE_MATERIAL_PASS_DEPTH
#define HE_MATERIAL_PASS_DEPTH 0
#endif
#ifndef HE_MATERIAL_PASS_SHADOW
#define HE_MATERIAL_PASS_SHADOW 0
#endif
#ifndef HE_MATERIAL_PASS_OUTLINE
#define HE_MATERIAL_PASS_OUTLINE 0
#endif
#ifndef HE_MATERIAL_SKINNED
#define HE_MATERIAL_SKINNED 0
#endif
#ifndef HE_MATERIAL_SHADING_UNLIT
#define HE_MATERIAL_SHADING_UNLIT 0
#endif
#ifndef HE_MATERIAL_BLEND_MASK
#define HE_MATERIAL_BLEND_MASK 0
#endif
#ifndef HE_MATERIAL_BLEND_TRANSPARENT
#define HE_MATERIAL_BLEND_TRANSPARENT 0
#endif

// Surface Material ABI v1 follows the existing SceneRenderer register spaces:
// space0 view/frame, space1 material textures, space2 object/material data,
// space3 pass/environment/shadow, and space4 material samplers.
cbuffer HEMaterialFrameConstants : register(b0, space0)
{
    float4x4 g_HEViewProjection;
    float4x4 g_HELightViewProjection;
    float4 g_HECameraPositionAndExposure;
    float4 g_HEDirectionalDirectionAndIntensity;
    float4 g_HEDirectionalColor;
    float4 g_HEPointPositionAndIntensity;
    float4 g_HEPointColorAndRadius;
    float4 g_HEShadowSettings;
    float4 g_HEIblSettings;
    float4x4 g_HEInverseViewProjection;
    float4x4 g_HEShadowCameraView;
    float4x4 g_HEAdditionalLightViewProjections[3];
    float4 g_HEDirectionalShadowCascadeSplits;
    float4 g_HEDirectionalShadowCascadeSettings;
    float4 g_HEDirectionalShadowDebugSettings;
    // x: monotonic presentation time in seconds; yzw reserved for ABI v1.
    float4 g_HEMaterialTimeAndPadding;
};

cbuffer HEMaterialObjectConstants : register(b0, space2)
{
    float4x4 g_HEWorld;
    float4x4 g_HEWorldInverseTranspose;
    uint4 g_HEMaterialObjectMeshIndices;
};

struct HEGpuMaterialData
{
    float4 BaseColor;
    float4 EmissiveAndMetallic;
    float4 RoughnessAoNormalAndPadding;
    uint4 MaterialTextureIndices;
    uint4 EmissiveSamplerFlags;
    // x: program generation, y: parameter row offset, z: row count, w: ABI version.
    uint4 ProgramParameterData;
};

StructuredBuffer<HEGpuMaterialData> g_HEGpuMaterials : register(t2, space2);
#if HE_MATERIAL_SKINNED
StructuredBuffer<float4x4> g_HEBoneMatrices : register(t3, space2);
#endif
StructuredBuffer<float4> g_HEMaterialParameters : register(t4, space2);

#if HE_MATERIAL_PASS_FORWARD
Texture2DArray<float> g_HEDirectionalShadow : register(t0, space3);
TextureCube<float4> g_HEIrradianceMap : register(t1, space3);
TextureCube<float4> g_HEPrefilterMap : register(t2, space3);
Texture2D<float2> g_HEBrdfLut : register(t3, space3);
SamplerState g_HEEnvironmentSampler : register(s0, space3);
#elif HE_MATERIAL_PASS_SHADOW
struct HEMaterialShadowPassConstants
{
    uint CascadeIndex;
    uint3 Padding;
};
HE_DECLARE_PUSH_CONSTANTS(
    HEMaterialShadowPassConstants,
    g_HEMaterialShadowPassConstants,
    space3);
#define g_HEShadowCascadeIndex g_HEMaterialShadowPassConstants.CascadeIndex
#endif

#define HE_MATERIAL_DEFAULT_SAMPLER \
    HE_MATERIAL_SAMPLER(g_HEGpuMaterials[g_HEMaterialObjectMeshIndices.x].EmissiveSamplerFlags.y)

struct HEMaterialInput
{
    float2 UV0;
    float3 PositionWS;
    float3 NormalWS;
    float3 TangentWS;
    float3 BitangentWS;
    float3 ViewWS;
    bool bFrontFace;
    // Monotonic presentation time in seconds, captured in the RenderWorld snapshot.
    float TimeSeconds;
};

struct HEMaterialSurface
{
    float3 BaseColor;
    float Metallic;
    float PerceptualRoughness;
    float AO;
    float3 NormalTS;
    float3 Emissive;
    float Opacity;
    float AlphaCutoff;
};

float4 HE_LoadMaterialFloat4(uint relativeRow)
{
    const HEGpuMaterialData material =
        g_HEGpuMaterials[g_HEMaterialObjectMeshIndices.x];
    if (relativeRow >= material.ProgramParameterData.z)
        return 0.0f;
    return g_HEMaterialParameters[material.ProgramParameterData.y + relativeRow];
}

float HE_LoadMaterialFloat(uint relativeRow)
{
    return HE_LoadMaterialFloat4(relativeRow).x;
}

float2 HE_LoadMaterialFloat2(uint relativeRow)
{
    return HE_LoadMaterialFloat4(relativeRow).xy;
}

float3 HE_LoadMaterialFloat3(uint relativeRow)
{
    return HE_LoadMaterialFloat4(relativeRow).xyz;
}

uint HE_LoadMaterialUint(uint relativeRow)
{
    return asuint(HE_LoadMaterialFloat4(relativeRow).x);
}

float3 HE_SafeNormalize(float3 value)
{
    if (!all(isfinite(value)))
        return float3(0.0f, 0.0f, 1.0f);
    const float lengthSquared = dot(value, value);
    return isfinite(lengthSquared) && lengthSquared > 1e-12f
        ? value * rsqrt(lengthSquared)
        : float3(0.0f, 0.0f, 1.0f);
}

float HE_SanitizeUnit(float value, float fallback)
{
    return isfinite(value) ? saturate(value) : fallback;
}

float3 HE_SanitizeHdr(float3 value)
{
    return all(isfinite(value)) ? clamp(value, 0.0f, 65504.0f) : 0.0f;
}

HEMaterialSurface HE_DefaultMaterialSurface()
{
    const HEGpuMaterialData material =
        g_HEGpuMaterials[g_HEMaterialObjectMeshIndices.x];
    HEMaterialSurface surface;
    surface.BaseColor = material.BaseColor.rgb;
    surface.Metallic = material.EmissiveAndMetallic.w;
    surface.PerceptualRoughness = material.RoughnessAoNormalAndPadding.x;
    surface.AO = material.RoughnessAoNormalAndPadding.y;
    surface.NormalTS = float3(0.0f, 0.0f, 1.0f);
    surface.Emissive = material.EmissiveAndMetallic.rgb;
    surface.Opacity = material.BaseColor.a;
    surface.AlphaCutoff = asfloat(material.EmissiveSamplerFlags.w);
    return surface;
}

float3x3 HE_BuildWorldTbn(HEMaterialInput input)
{
    const float faceSign = input.bFrontFace ? 1.0f : -1.0f;
    const float3 sourceNormal = HE_SafeNormalize(input.NormalWS);
    const float3 orthogonalTangent =
        input.TangentWS - sourceNormal * dot(sourceNormal, input.TangentWS);
    const float3 fallbackTangent = abs(sourceNormal.z) < 0.999f
        ? HE_SafeNormalize(cross(float3(0.0f, 0.0f, 1.0f), sourceNormal))
        : HE_SafeNormalize(cross(float3(0.0f, 1.0f, 0.0f), sourceNormal));
    const float3 tangent = HE_SafeNormalize(
        dot(orthogonalTangent, orthogonalTangent) > 1e-8f
            ? orthogonalTangent
            : fallbackTangent);
    const float handedness = dot(cross(sourceNormal, tangent), input.BitangentWS) < 0.0f
        ? -1.0f
        : 1.0f;
    const float3 normal = sourceNormal * faceSign;
    const float3 bitangent = HE_SafeNormalize(cross(normal, tangent)) * handedness;
    return float3x3(tangent, bitangent, normal);
}

float3 HE_ResolveMaterialNormal(HEMaterialInput input, float3 normalTS)
{
    normalTS = all(isfinite(normalTS))
        ? HE_SafeNormalize(normalTS)
        : float3(0.0f, 0.0f, 1.0f);
    const float3x3 tangentBasis = HE_BuildWorldTbn(input);
    return HE_SafeNormalize(
        normalTS.x * tangentBasis[0] +
        normalTS.y * tangentBasis[1] +
        normalTS.z * tangentBasis[2]);
}

float HE_ApplySpecularAntiAliasing(float roughness, float3 normalWS)
{
    const float3 dndx = ddx(normalWS);
    const float3 dndy = ddy(normalWS);
    const float normalVariance = all(isfinite(dndx)) && all(isfinite(dndy))
        ? min((dot(dndx, dndx) + dot(dndy, dndy)) / 12.0f, 0.35f)
        : 0.35f;
    const float minimumAlpha = 0.08f * 0.08f;
    const float materialAlpha = max(roughness * roughness, minimumAlpha);
    return sqrt(saturate(materialAlpha + max(normalVariance, 0.0f)));
}

float HE_DistributionGGX(float3 normalWS, float3 halfwayWS, float roughness)
{
    const float alpha = roughness * roughness;
    const float alphaSquared = alpha * alpha;
    const float nDotH = saturate(dot(normalWS, halfwayWS));
    const float denominator = nDotH * nDotH * (alphaSquared - 1.0f) + 1.0f;
    return alphaSquared / max(3.14159265f * denominator * denominator, 1e-12f);
}

float HE_GeometrySchlickGGX(float nDotV, float roughness)
{
    const float r = roughness + 1.0f;
    const float k = (r * r) / 8.0f;
    return nDotV / max(nDotV * (1.0f - k) + k, 1e-5f);
}

float3 HE_FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0f - f0) * pow(1.0f - saturate(cosTheta), 5.0f);
}

float3 HE_FresnelSchlickRoughness(float cosTheta, float3 f0, float roughness)
{
    return f0 + (max((1.0f - roughness).xxx, f0) - f0) *
        pow(1.0f - saturate(cosTheta), 5.0f);
}

float3 HE_EvaluateLight(
    HEMaterialSurface surface,
    float3 normalWS,
    float3 viewWS,
    float3 lightWS,
    float3 radiance)
{
    const float nDotL = saturate(dot(normalWS, lightWS));
    if (nDotL <= 0.0f)
        return 0.0f;
    const float3 halfwayWS = HE_SafeNormalize(viewWS + lightWS);
    const float nDotV = saturate(dot(normalWS, viewWS));
    const float3 f0 = lerp(0.04f.xxx, surface.BaseColor, surface.Metallic);
    const float3 fresnel = HE_FresnelSchlick(saturate(dot(halfwayWS, viewWS)), f0);
    const float geometry = HE_GeometrySchlickGGX(nDotV, surface.PerceptualRoughness) *
        HE_GeometrySchlickGGX(nDotL, surface.PerceptualRoughness);
    const float distribution = HE_DistributionGGX(
        normalWS, halfwayWS, surface.PerceptualRoughness);
    const float3 specular = (distribution * geometry * fresnel) /
        max(4.0f * nDotV * nDotL, 1e-4f);
    const float3 diffuseWeight = (1.0f - fresnel) * (1.0f - surface.Metallic);
    return (diffuseWeight * surface.BaseColor / 3.14159265f + specular) *
        radiance * nDotL;
}

float HE_ComputePointLightAttenuation(
    float distanceToLight,
    float radius,
    float falloffExponent)
{
    const float safeRadius = max(radius, 1e-3f);
    const float safeDistance = max(distanceToLight, 1e-2f);
    const float safeExponent = clamp(falloffExponent, 0.0f, 8.0f);
    const float normalizedDistance = distanceToLight / safeRadius;
    const float falloff = saturate(
        1.0f - normalizedDistance * normalizedDistance *
        normalizedDistance * normalizedDistance);
    return (falloff * falloff) / max(pow(safeDistance, safeExponent), 1e-4f);
}

#if HE_MATERIAL_PASS_FORWARD
float4x4 HE_GetDirectionalShadowMatrix(int cascadeIndex)
{
    return cascadeIndex == 0
        ? g_HELightViewProjection
        : g_HEAdditionalLightViewProjections[min(cascadeIndex - 1, 2)];
}

int HE_SelectDirectionalShadowCascade(float viewDepth)
{
    const int cascadeCount = clamp(
        (int)round(g_HEDirectionalShadowCascadeSettings.x), 1, 4);
    [unroll]
    for (int cascade = 0; cascade < 4; ++cascade)
    {
        if (cascade >= cascadeCount)
            break;
        if (viewDepth <= g_HEDirectionalShadowCascadeSplits[cascade])
            return cascade;
    }
    return -1;
}

float HE_SampleDirectionalShadowCascade(
    float3 positionWS,
    float3 geometricNormalWS,
    float3 lightWS,
    int cascadeIndex)
{
    const float normalBiasScale = 1.0f - saturate(dot(geometricNormalWS, lightWS));
    const float3 biasedPositionWS = positionWS +
        geometricNormalWS * g_HEShadowSettings.z * normalBiasScale;
    const float4 shadowClip = mul(
        HE_GetDirectionalShadowMatrix(cascadeIndex),
        float4(biasedPositionWS, 1.0f));
    if (shadowClip.w <= 0.0f)
        return 1.0f;
    const float3 shadowNdc = shadowClip.xyz / shadowClip.w;
    const float2 shadowUv = float2(
        shadowNdc.x * 0.5f + 0.5f,
        0.5f - shadowNdc.y * 0.5f);
    if (any(shadowUv < 0.0f) || any(shadowUv > 1.0f) ||
        shadowNdc.z < 0.0f || shadowNdc.z > 1.0f)
        return 1.0f;

    uint shadowWidth = 0;
    uint shadowHeight = 0;
    uint shadowLayers = 0;
    g_HEDirectionalShadow.GetDimensions(shadowWidth, shadowHeight, shadowLayers);
    if ((uint)cascadeIndex >= shadowLayers)
        return 1.0f;
    const float compareDepth = saturate(shadowNdc.z - g_HEShadowSettings.y);
    const int radius = clamp((int)round(g_HEShadowSettings.w), 0, 4);
    const int2 maximumTexel = int2(
        max((int)shadowWidth - 1, 0),
        max((int)shadowHeight - 1, 0));
    const int2 centerTexel = clamp(
        int2(shadowUv * float2(shadowWidth, shadowHeight)),
        int2(0, 0), maximumTexel);
    float visibility = 0.0f;
    float sampleCount = 0.0f;
    [loop]
    for (int y = -radius; y <= radius; ++y)
    {
        [loop]
        for (int x = -radius; x <= radius; ++x)
        {
            const int2 sampleTexel = clamp(
                centerTexel + int2(x, y), int2(0, 0), maximumTexel);
            const float shadowDepth = g_HEDirectionalShadow.Load(
                int4(sampleTexel, cascadeIndex, 0));
            visibility += compareDepth <= shadowDepth ? 1.0f : 0.0f;
            sampleCount += 1.0f;
        }
    }
    return visibility / max(sampleCount, 1.0f);
}

float HE_EvaluateDirectionalShadow(
    float3 positionWS,
    float3 geometricNormalWS,
    float3 lightWS)
{
    if (g_HEShadowSettings.x <= 0.0f)
        return 1.0f;
    const float viewDepth = abs(mul(
        g_HEShadowCameraView, float4(positionWS, 1.0f)).z);
    const int cascadeIndex = HE_SelectDirectionalShadowCascade(viewDepth);
    if (cascadeIndex < 0)
        return 1.0f;
    const int cascadeCount = clamp(
        (int)round(g_HEDirectionalShadowCascadeSettings.x), 1, 4);
    float visibility = HE_SampleDirectionalShadowCascade(
        positionWS, geometricNormalWS, lightWS, cascadeIndex);
    const float previousSplit = cascadeIndex > 0
        ? g_HEDirectionalShadowCascadeSplits[cascadeIndex - 1]
        : g_HEDirectionalShadowDebugSettings.w;
    const float cascadeSpan = max(
        g_HEDirectionalShadowCascadeSplits[cascadeIndex] - previousSplit,
        1e-3f);
    if (cascadeIndex + 1 < cascadeCount)
    {
        const float blendRange = cascadeSpan *
            saturate(g_HEDirectionalShadowCascadeSettings.z);
        const float blendStart =
            g_HEDirectionalShadowCascadeSplits[cascadeIndex] - blendRange;
        if (blendRange > 0.0f && viewDepth > blendStart)
        {
            visibility = lerp(
                visibility,
                HE_SampleDirectionalShadowCascade(
                    positionWS, geometricNormalWS, lightWS, cascadeIndex + 1),
                smoothstep(
                    blendStart,
                    g_HEDirectionalShadowCascadeSplits[cascadeIndex],
                    viewDepth));
        }
    }
    else
    {
        const float fadeRange = cascadeSpan *
            saturate(g_HEDirectionalShadowCascadeSettings.w);
        const float fadeStart =
            g_HEDirectionalShadowCascadeSplits[cascadeIndex] - fadeRange;
        if (fadeRange > 0.0f && viewDepth > fadeStart)
        {
            visibility = lerp(
                visibility,
                1.0f,
                smoothstep(
                    fadeStart,
                    g_HEDirectionalShadowCascadeSplits[cascadeIndex],
                    viewDepth));
        }
    }
    return lerp(1.0f, visibility, saturate(g_HEShadowSettings.x));
}

float3 HE_EvaluateIbl(
    HEMaterialSurface surface,
    float3 normalWS,
    float3 viewWS)
{
    if (g_HEIblSettings.z <= 0.0f)
        return 0.0f;
    const float nDotV = max(saturate(dot(normalWS, viewWS)), 1e-4f);
    const float3 f0 = lerp(0.04f.xxx, surface.BaseColor, surface.Metallic);
    const float3 fresnel = HE_FresnelSchlickRoughness(
        nDotV, f0, surface.PerceptualRoughness);
    const float3 diffuseWeight = (1.0f - fresnel) * (1.0f - surface.Metallic);
    const float3 irradiance = g_HEIrradianceMap.SampleLevel(
        g_HEEnvironmentSampler, normalWS, 0.0f).rgb;
    const float3 reflectionWS = reflect(-viewWS, normalWS);
    const float3 prefiltered = g_HEPrefilterMap.SampleLevel(
        g_HEEnvironmentSampler,
        reflectionWS,
        surface.PerceptualRoughness * g_HEIblSettings.y).rgb;
    const float2 brdf = g_HEBrdfLut.SampleLevel(
        g_HEEnvironmentSampler,
        float2(nDotV, surface.PerceptualRoughness),
        0.0f).rg;
    const float exponent = exp2(-16.0f * surface.PerceptualRoughness - 1.0f);
    const float specularOcclusion = saturate(
        pow(max(nDotV + surface.AO, 1e-4f), exponent) - 1.0f + surface.AO);
    const float3 diffuse = diffuseWeight * irradiance *
        surface.BaseColor * surface.AO;
    const float3 specular = prefiltered *
        (f0 * brdf.x + brdf.y) * specularOcclusion;
    return (diffuse + specular) * g_HEIblSettings.x;
}

float3 HE_ShadeMaterial(HEMaterialInput input, HEMaterialSurface surface)
{
    surface.BaseColor = saturate(surface.BaseColor);
    surface.Metallic = HE_SanitizeUnit(surface.Metallic, 0.0f);
    surface.PerceptualRoughness = isfinite(surface.PerceptualRoughness)
        ? clamp(surface.PerceptualRoughness, 0.045f, 1.0f)
        : 1.0f;
    surface.AO = HE_SanitizeUnit(surface.AO, 1.0f);
    surface.Emissive = HE_SanitizeHdr(surface.Emissive);
#if HE_MATERIAL_SHADING_UNLIT
    return HE_SanitizeHdr(surface.BaseColor + surface.Emissive);
#else
    const float3 normalWS = HE_ResolveMaterialNormal(input, surface.NormalTS);
    surface.PerceptualRoughness = HE_ApplySpecularAntiAliasing(
        surface.PerceptualRoughness,
        normalWS);
    const float3 geometricNormalWS = HE_SafeNormalize(input.NormalWS) *
        (input.bFrontFace ? 1.0f : -1.0f);
    float3 color = HE_EvaluateIbl(surface, normalWS, input.ViewWS);
    if (g_HEDirectionalDirectionAndIntensity.w > 0.0f)
    {
        const float3 lightWS = HE_SafeNormalize(
            -g_HEDirectionalDirectionAndIntensity.xyz);
        const float shadow = HE_EvaluateDirectionalShadow(
            input.PositionWS, geometricNormalWS, lightWS);
        color += shadow * HE_EvaluateLight(
            surface,
            normalWS,
            input.ViewWS,
            lightWS,
            g_HEDirectionalColor.rgb * g_HEDirectionalDirectionAndIntensity.w);
    }
    if (g_HEPointPositionAndIntensity.w > 0.0f && g_HEPointColorAndRadius.w > 0.0f)
    {
        const float3 toLight = g_HEPointPositionAndIntensity.xyz - input.PositionWS;
        const float distanceToLight = length(toLight);
        const float attenuation = HE_ComputePointLightAttenuation(
            distanceToLight,
            g_HEPointColorAndRadius.w,
            g_HEDirectionalShadowDebugSettings.z);
        color += HE_EvaluateLight(
            surface,
            normalWS,
            input.ViewWS,
            HE_SafeNormalize(toLight),
            g_HEPointColorAndRadius.rgb *
                g_HEPointPositionAndIntensity.w * attenuation);
    }
    return HE_SanitizeHdr(color + surface.Emissive);
#endif
}
#endif

#if HE_MATERIAL_PASS_SHADOW
float4x4 HE_GetShadowPassMatrix(uint cascadeIndex)
{
    return cascadeIndex == 0u
        ? g_HELightViewProjection
        : g_HEAdditionalLightViewProjections[min(cascadeIndex - 1u, 2u)];
}
#endif

#endif
