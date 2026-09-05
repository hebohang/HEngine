#pragma htype vs VS
#pragma htype ps PS
#pragma hvariant GPU_DRIVEN
#pragma hvariant SKINNED

#include "MaterialDescriptorAccess.hlsli"
#include "Shared/BackendPushConstants.hlsli"

// Tiered SceneRenderer binding contract:
// space0: per-view data, space1: bounded tables or global descriptor heaps,
// space2: per-object/material constants. Pixel output stays linear HDR.
cbuffer FrameConstants : register(b0, space0)
{
    float4x4 g_ViewProjection;
    float4x4 g_LightViewProjection;
    float4 g_CameraPositionAndExposure;
    float4 g_DirectionalDirectionAndIntensity;
    float4 g_DirectionalColor;
    float4 g_PointPositionAndIntensity;
    float4 g_PointColorAndRadius;
    // x: strength, y: constant bias, z: normal bias, w: PCF radius in texels.
    float4 g_ShadowSettings;
    // x: IBL intensity, y: prefilter max mip, z: resources ready.
    float4 g_IblSettings;
    float4x4 g_InverseViewProjection;
    float4x4 g_ShadowCameraView;
    float4x4 g_AdditionalLightViewProjections[3];
    float4 g_DirectionalShadowCascadeSplits;
    // x: cascade count, y: map size, z: blend fraction, w: far fade fraction.
    float4 g_DirectionalShadowCascadeSettings;
    // x: cascade debug, y: PBR debug mode, z: first point falloff exponent,
    // w: sanitized camera near plane used by the first cascade blend span.
    float4 g_DirectionalShadowDebugSettings;
};

Texture2DArray<float> g_DirectionalShadow : register(t0, space3);
TextureCube<float4> g_IrradianceMap : register(t1, space3);
TextureCube<float4> g_PrefilterMap : register(t2, space3);
Texture2D<float2> g_BrdfLut : register(t3, space3);
SamplerState g_ShadowSampler : register(s0, space3);

static const uint c_PbrDebugBaseColor = 1u;
static const uint c_PbrDebugWorldNormal = 2u;
static const uint c_PbrDebugMetallic = 3u;
static const uint c_PbrDebugRoughness = 4u;
static const uint c_PbrDebugAo = 5u;
static const uint c_PbrDebugDirect = 6u;
static const uint c_PbrDebugDiffuseIbl = 7u;
static const uint c_PbrDebugSpecularIbl = 8u;
static const uint c_PbrDebugShadow = 9u;
static const float c_Pi = 3.14159265358979323846f;
static const float c_MaxFiniteHalf = 65504.0f;
// A raster pixel cannot resolve an arbitrarily narrow microfacet lobe. Keep
// the renderer's effective lobe finite, then add measured normal variance.
static const float c_MinResolvedPerceptualRoughness = 0.08f;
static const float c_MaxNormalVariance = 0.35f;

cbuffer ObjectConstants : register(b0, space2)
{
    float4x4 g_World;
    float4x4 g_WorldInverseTranspose;
    // x: GPU material, y: object, z: mesh, w: reserved.
    uint4 g_MaterialObjectMeshIndices;
};

struct GpuObjectData
{
    float4x4 World;
    float4x4 WorldInverseTranspose;
    uint4 MaterialObjectMeshIndices;
};

struct GpuMaterialData
{
    float4 BaseColor;
    float4 EmissiveAndMetallic;
    float4 RoughnessAoNormalAndPadding;
    uint4 MaterialTextureIndices;
    uint4 EmissiveSamplerFlags;
    uint4 ProgramParameterData;
};

StructuredBuffer<GpuMaterialData> g_GpuMaterials : register(t2, space2);
#if SKINNED
StructuredBuffer<float4x4> g_BoneMatrices : register(t3, space2);
#endif

#if GPU_DRIVEN
StructuredBuffer<GpuObjectData> g_GpuObjects : register(t0, space2);
StructuredBuffer<uint> g_VisibleObjectIndices : register(t1, space2);
struct GpuDrawConstants
{
    uint VisibleBase;
    uint3 Padding;
};
HE_DECLARE_PUSH_CONSTANTS_AT(GpuDrawConstants, g_GpuDrawConstants, 1, space2);

#define g_VisibleBase g_GpuDrawConstants.VisibleBase
#endif

struct VertexInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD;
    float3 Tangent : TANGENT;
    float3 Bitangent : BINORMAL;
#if SKINNED
    int4 BoneIds : BONEIDS;
    float4 BoneWeights : BONEWEIGHTS;
#endif
};

struct SurfaceVaryings
{
    float4 PositionCS : SV_Position;
    float3 PositionWS : POSITION0;
    float3 NormalWS : NORMAL0;
    float3 TangentWS : TANGENT0;
    float3 BitangentWS : BINORMAL0;
    float2 TexCoord : TEXCOORD0;
#if GPU_DRIVEN
    nointerpolation uint ObjectIndex : TEXCOORD1;
#endif
};

SurfaceVaryings VS(VertexInput input, uint instanceId : SV_InstanceID)
{
#if GPU_DRIVEN
    const uint objectIndex = g_VisibleObjectIndices[g_VisibleBase + instanceId];
    const GpuObjectData gpuObject = g_GpuObjects[objectIndex];
    const float4x4 world = gpuObject.World;
    const float4x4 worldInverseTranspose = gpuObject.WorldInverseTranspose;
#else
    const float4x4 world = g_World;
    const float4x4 worldInverseTranspose = g_WorldInverseTranspose;
#endif
    SurfaceVaryings output;
    float4 positionLS = float4(input.Position, 1.0f);
    float3 normalLS = input.Normal;
    float3 tangentLS = input.Tangent;
    float3 bitangentLS = input.Bitangent;
#if SKINNED
    float4 skinnedPosition = 0.0f;
    float3 skinnedNormal = 0.0f;
    float3 skinnedTangent = 0.0f;
    float3 skinnedBitangent = 0.0f;
    float totalWeight = 0.0f;
    [unroll]
    for (uint influence = 0; influence < 4; ++influence)
    {
        const int boneId = input.BoneIds[influence];
        const float weight = input.BoneWeights[influence];
        if (boneId < 0 || weight <= 0.0f)
            continue;
        const float4x4 bone = g_BoneMatrices[g_MaterialObjectMeshIndices.w + boneId];
        skinnedPosition += mul(bone, positionLS) * weight;
        skinnedNormal += mul((float3x3)bone, normalLS) * weight;
        skinnedTangent += mul((float3x3)bone, tangentLS) * weight;
        skinnedBitangent += mul((float3x3)bone, bitangentLS) * weight;
        totalWeight += weight;
    }
    if (totalWeight > 0.0f)
    {
        positionLS = skinnedPosition / totalWeight;
        normalLS = skinnedNormal / totalWeight;
        tangentLS = skinnedTangent / totalWeight;
        bitangentLS = skinnedBitangent / totalWeight;
    }
#endif
    const float4 positionWS = mul(world, positionLS);
    output.PositionCS = mul(g_ViewProjection, positionWS);
    output.PositionWS = positionWS.xyz;
    output.NormalWS = normalize(mul((float3x3)worldInverseTranspose, normalLS));
    output.TangentWS = normalize(mul((float3x3)world, tangentLS));
    output.BitangentWS = normalize(mul((float3x3)world, bitangentLS));
    output.TexCoord = input.TexCoord;
#if GPU_DRIVEN
    output.ObjectIndex = objectIndex;
#endif
    return output;
}

float DistributionGGX(float3 normalWS, float3 halfwayWS, float roughness)
{
    const float alpha = roughness * roughness;
    const float alphaSquared = alpha * alpha;
    const float nDotH = saturate(dot(normalWS, halfwayWS));
    const float denominator = nDotH * nDotH * (alphaSquared - 1.0f) + 1.0f;
    return alphaSquared / max(c_Pi * denominator * denominator, 1e-12f);
}

float GeometrySchlickGGX(float nDotV, float roughness)
{
    const float r = roughness + 1.0f;
    const float k = (r * r) / 8.0f;
    return nDotV / max(nDotV * (1.0f - k) + k, 1e-5f);
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0f - f0) * pow(1.0f - saturate(cosTheta), 5.0f);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 f0, float roughness)
{
    return f0 + (max((1.0f - roughness).xxx, f0) - f0) *
        pow(1.0f - saturate(cosTheta), 5.0f);
}

float3 SafeNormalize(float3 value)
{
    if (!all(isfinite(value)))
        return float3(0.0f, 0.0f, 1.0f);
    const float lengthSquared = dot(value, value);
    return isfinite(lengthSquared) && lengthSquared > 1e-12f
        ? value * rsqrt(lengthSquared)
        : float3(0.0f, 0.0f, 1.0f);
}

float3 SanitizeHalfRadiance(float3 value)
{
    if (!all(isfinite(value)))
        return 0.0f;
    return clamp(value, 0.0f, c_MaxFiniteHalf);
}

float SanitizeUnit(float value, float fallback)
{
    return isfinite(value) ? saturate(value) : fallback;
}

float3x3 BuildWorldTbn(
    float3 normalWS,
    float3 tangentWS,
    float3 bitangentWS,
    float faceSign)
{
    const float3 sourceNormal = SafeNormalize(normalWS);
    const float3 orthogonalTangent =
        tangentWS - sourceNormal * dot(sourceNormal, tangentWS);
    const float3 fallbackTangent = abs(sourceNormal.z) < 0.999f
        ? SafeNormalize(cross(float3(0.0f, 0.0f, 1.0f), sourceNormal))
        : SafeNormalize(cross(float3(0.0f, 1.0f, 0.0f), sourceNormal));
    const float3 tangent = SafeNormalize(
        dot(orthogonalTangent, orthogonalTangent) > 1e-8f
            ? orthogonalTangent
            : fallbackTangent);
    const float handedness = dot(cross(sourceNormal, tangent), bitangentWS) < 0.0f
        ? -1.0f
        : 1.0f;
    const float3 normal = sourceNormal * faceSign;
    const float3 bitangent = SafeNormalize(cross(normal, tangent)) * handedness;
    return float3x3(tangent, bitangent, normal);
}

float3 SampleMaterialNormal(
    Texture2D<float4> normalTexture,
    SamplerState materialSampler,
    float2 texCoord,
    float3x3 tangentBasis,
    float normalScale,
    out float normalMipVariance)
{
    // Linear normal-map mips retain the length of the averaged tangent-space
    // vector. Its lost length estimates unresolved slope variance (Toksvig),
    // instead of hiding aliasing with a fixed mip bias.
    const float3 averagedNormalTS =
        normalTexture.Sample(materialSampler, texCoord).xyz * 2.0f - 1.0f;
    if (!all(isfinite(averagedNormalTS)))
    {
        normalMipVariance = 0.0f;
        return tangentBasis[2];
    }
    const float averageNormalLength = length(averagedNormalTS);
    normalMipVariance = averageNormalLength > 1e-4f
        ? min(
            max(1.0f - averageNormalLength, 0.0f) / averageNormalLength,
            c_MaxNormalVariance)
        : c_MaxNormalVariance;

    const float safeNormalScale = isfinite(normalScale)
        ? clamp(normalScale, -8.0f, 8.0f)
        : 1.0f;
    normalMipVariance = min(
        normalMipVariance * safeNormalScale * safeNormalScale,
        c_MaxNormalVariance);
    float2 normalXY = averagedNormalTS.xy * safeNormalScale;
    const float xyLengthSquared = dot(normalXY, normalXY);
    if (xyLengthSquared > 0.999f)
        normalXY *= sqrt(0.999f / xyLengthSquared);

    const float3 normalTS = float3(
        normalXY,
        sqrt(max(1.0f - dot(normalXY, normalXY), 1e-4f)));
    return SafeNormalize(
        normalTS.x * tangentBasis[0] +
        normalTS.y * tangentBasis[1] +
        normalTS.z * tangentBasis[2]);
}

float ApplySpecularAntiAliasing(
    float roughness,
    float3 normalWS,
    float normalMipVariance)
{
    const float3 dndx = ddx(normalWS);
    const float3 dndy = ddy(normalWS);
    // For a box-filtered pixel, the variance of a linear normal ramp is
    // derivative^2 / 12. Add that footprint variance to the mip-derived
    // variance in GGX alpha space (alpha = perceptualRoughness^2).
    const float screenNormalVariance =
        all(isfinite(dndx)) && all(isfinite(dndy))
        ? (dot(dndx, dndx) + dot(dndy, dndy)) / 12.0f
        : c_MaxNormalVariance;
    const float normalVariance = min(
        max(normalMipVariance, 0.0f) + max(screenNormalVariance, 0.0f),
        c_MaxNormalVariance);
    const float minimumAlpha = c_MinResolvedPerceptualRoughness *
        c_MinResolvedPerceptualRoughness;
    const float materialAlpha = max(roughness * roughness, minimumAlpha);
    return sqrt(saturate(materialAlpha + normalVariance));
}

float ComputeSpecularOcclusion(float nDotV, float ao, float roughness)
{
    const float exponent = exp2(-16.0f * roughness - 1.0f);
    return saturate(pow(max(nDotV + ao, 1e-4f), exponent) - 1.0f + ao);
}

float ComputePointLightAttenuation(
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
    return (falloff * falloff) /
        max(pow(safeDistance, safeExponent), 1e-4f);
}

float3 EvaluateLight(
    float3 baseColor,
    float metallic,
    float roughness,
    float3 normalWS,
    float3 viewWS,
    float3 lightWS,
    float3 radiance)
{
    const float nDotL = saturate(dot(normalWS, lightWS));
    if (nDotL <= 0.0f)
        return 0.0f;

    const float3 halfwayWS = SafeNormalize(viewWS + lightWS);
    const float nDotV = saturate(dot(normalWS, viewWS));
    const float3 f0 = lerp(0.04f.xxx, baseColor, metallic);
    const float3 fresnel = FresnelSchlick(saturate(dot(halfwayWS, viewWS)), f0);
    const float geometry = GeometrySchlickGGX(nDotV, roughness) *
        GeometrySchlickGGX(nDotL, roughness);
    const float distribution = DistributionGGX(normalWS, halfwayWS, roughness);
    const float3 specular =
        (distribution * geometry * fresnel) / max(4.0f * nDotV * nDotL, 1e-4f);
    const float3 diffuseWeight = (1.0f - fresnel) * (1.0f - metallic);
    return (diffuseWeight * baseColor / 3.14159265f + specular) * radiance * nDotL;
}

float4x4 GetDirectionalShadowMatrix(int cascadeIndex)
{
    if (cascadeIndex == 0)
        return g_LightViewProjection;
    return g_AdditionalLightViewProjections[min(cascadeIndex - 1, 2)];
}

float3 GetDirectionalShadowCascadeDebugColor(int cascadeIndex)
{
    if (cascadeIndex == 0)
        return float3(1.0f, 0.25f, 0.2f);
    if (cascadeIndex == 1)
        return float3(0.25f, 1.0f, 0.25f);
    if (cascadeIndex == 2)
        return float3(0.25f, 0.45f, 1.0f);
    return float3(1.0f, 0.85f, 0.2f);
}

int SelectDirectionalShadowCascade(float viewDepth)
{
    const int cascadeCount = clamp(
        (int)round(g_DirectionalShadowCascadeSettings.x),
        1,
        4);
    [unroll]
    for (int cascade = 0; cascade < 4; ++cascade)
    {
        if (cascade >= cascadeCount)
            break;
        if (viewDepth <= g_DirectionalShadowCascadeSplits[cascade])
            return cascade;
    }
    return -1;
}

float SampleDirectionalShadowCascade(
    float3 positionWS,
    float3 geometricNormalWS,
    float3 lightWS,
    int cascadeIndex)
{
    const float normalBiasScale =
        1.0f - saturate(dot(geometricNormalWS, lightWS));
    const float3 biasedPositionWS = positionWS +
        geometricNormalWS * g_ShadowSettings.z * normalBiasScale;
    const float4 shadowClip = mul(
        GetDirectionalShadowMatrix(cascadeIndex),
        float4(biasedPositionWS, 1.0f));
    if (shadowClip.w <= 0.0f)
        return 1.0f;

    const float3 shadowNdc = shadowClip.xyz / shadowClip.w;
    const float2 shadowUv = float2(
        shadowNdc.x * 0.5f + 0.5f,
        0.5f - shadowNdc.y * 0.5f);
    if (any(shadowUv < 0.0f) || any(shadowUv > 1.0f) ||
        shadowNdc.z < 0.0f || shadowNdc.z > 1.0f)
    {
        return 1.0f;
    }

    uint shadowWidth = 0;
    uint shadowHeight = 0;
    uint shadowLayers = 0;
    g_DirectionalShadow.GetDimensions(shadowWidth, shadowHeight, shadowLayers);
    if ((uint)cascadeIndex >= shadowLayers)
        return 1.0f;
    const float compareDepth = saturate(shadowNdc.z - g_ShadowSettings.y);
    const int radius = clamp((int)round(g_ShadowSettings.w), 0, 4);
    const int2 maximumTexel = int2(
        max((int)shadowWidth - 1, 0),
        max((int)shadowHeight - 1, 0));
    const int2 centerTexel = clamp(
        int2(shadowUv * float2(shadowWidth, shadowHeight)),
        int2(0, 0),
        maximumTexel);
    float visibility = 0.0f;
    float sampleCount = 0.0f;
    [loop]
    for (int y = -radius; y <= radius; ++y)
    {
        [loop]
        for (int x = -radius; x <= radius; ++x)
        {
            const int2 sampleTexel = clamp(
                centerTexel + int2(x, y),
                int2(0, 0),
                maximumTexel);
            const float shadowDepth = g_DirectionalShadow.Load(
                int4(sampleTexel, cascadeIndex, 0));
            visibility += compareDepth <= shadowDepth ? 1.0f : 0.0f;
            sampleCount += 1.0f;
        }
    }
    return visibility / max(sampleCount, 1.0f);
}

float EvaluateDirectionalShadow(
    float3 positionWS,
    float3 geometricNormalWS,
    float3 lightWS,
    out float3 cascadeDebugColor)
{
    cascadeDebugColor = 1.0f;
    if (g_ShadowSettings.x <= 0.0f)
        return 1.0f;

    const float viewDepth = abs(mul(
        g_ShadowCameraView,
        float4(positionWS, 1.0f)).z);
    const int cascadeIndex = SelectDirectionalShadowCascade(viewDepth);
    if (cascadeIndex < 0)
        return 1.0f;

    const int cascadeCount = clamp(
        (int)round(g_DirectionalShadowCascadeSettings.x),
        1,
        4);
    float visibility = SampleDirectionalShadowCascade(
        positionWS,
        geometricNormalWS,
        lightWS,
        cascadeIndex);
    cascadeDebugColor = GetDirectionalShadowCascadeDebugColor(cascadeIndex);

    const float previousSplit = cascadeIndex > 0
        ? g_DirectionalShadowCascadeSplits[cascadeIndex - 1]
        : g_DirectionalShadowDebugSettings.w;
    const float cascadeSpan = max(
        g_DirectionalShadowCascadeSplits[cascadeIndex] - previousSplit,
        1e-3f);
    if (cascadeIndex + 1 < cascadeCount)
    {
        const float blendRange = cascadeSpan *
            saturate(g_DirectionalShadowCascadeSettings.z);
        const float blendStart =
            g_DirectionalShadowCascadeSplits[cascadeIndex] - blendRange;
        if (blendRange > 0.0f && viewDepth > blendStart)
        {
            const float blend = smoothstep(
                blendStart,
                g_DirectionalShadowCascadeSplits[cascadeIndex],
                viewDepth);
            visibility = lerp(
                visibility,
                SampleDirectionalShadowCascade(
                    positionWS,
                    geometricNormalWS,
                    lightWS,
                    cascadeIndex + 1),
                blend);
            cascadeDebugColor = lerp(
                cascadeDebugColor,
                GetDirectionalShadowCascadeDebugColor(cascadeIndex + 1),
                blend);
        }
    }
    else
    {
        const float fadeRange = cascadeSpan *
            saturate(g_DirectionalShadowCascadeSettings.w);
        const float fadeStart =
            g_DirectionalShadowCascadeSplits[cascadeIndex] - fadeRange;
        if (fadeRange > 0.0f && viewDepth > fadeStart)
        {
            const float fade = smoothstep(
                fadeStart,
                g_DirectionalShadowCascadeSplits[cascadeIndex],
                viewDepth);
            visibility = lerp(visibility, 1.0f, fade);
            cascadeDebugColor = lerp(cascadeDebugColor, 1.0f, fade);
        }
    }
    return lerp(1.0f, visibility, saturate(g_ShadowSettings.x));
}

float4 PS(SurfaceVaryings input, bool bIsFrontFace : SV_IsFrontFace) : SV_Target0
{
#if GPU_DRIVEN
    const GpuObjectData gpuObject = g_GpuObjects[input.ObjectIndex];
    const uint materialIndex = gpuObject.MaterialObjectMeshIndices.x;
#else
    const uint materialIndex = g_MaterialObjectMeshIndices.x;
#endif
    const GpuMaterialData material = g_GpuMaterials[materialIndex];
    const float4 objectBaseColor = material.BaseColor;
    const float4 objectEmissiveAndMetallic = material.EmissiveAndMetallic;
    const float4 objectRoughnessAoNormalAndObjectIndex =
        material.RoughnessAoNormalAndPadding;
    const uint4 materialTextureIndices = material.MaterialTextureIndices;
    const uint4 emissiveSamplerFlags = material.EmissiveSamplerFlags;
    Texture2D<float4> baseColorTexture = HE_MATERIAL_TEXTURE(materialTextureIndices.x);
    Texture2D<float4> normalTexture = HE_MATERIAL_TEXTURE(materialTextureIndices.y);
    Texture2D<float4> metallicRoughnessTexture = HE_MATERIAL_TEXTURE(materialTextureIndices.z);
    Texture2D<float4> occlusionTexture = HE_MATERIAL_TEXTURE(materialTextureIndices.w);
    Texture2D<float4> emissiveTexture = HE_MATERIAL_TEXTURE(emissiveSamplerFlags.x);
    SamplerState materialSampler = HE_MATERIAL_SAMPLER(emissiveSamplerFlags.y);

    // Base/emissive textures use SRGB resource formats, so sampling returns
    // linear values. Normal, metallic/roughness, and occlusion remain linear.
    const float4 baseSample = baseColorTexture.Sample(materialSampler, input.TexCoord);
    const uint materialFlags = emissiveSamplerFlags.z;
    const float alpha = SanitizeUnit(baseSample.a * objectBaseColor.a, 0.0f);
    if ((materialFlags & 1u) != 0u)
        clip(alpha - asfloat(emissiveSamplerFlags.w));
    const float3 baseColor = baseSample.rgb * objectBaseColor.rgb;
    const float faceSign = bIsFrontFace ? 1.0f : -1.0f;
    const float3 geometricNormal = SafeNormalize(input.NormalWS) * faceSign;
    const float3x3 tangentBasis = BuildWorldTbn(
        input.NormalWS,
        input.TangentWS,
        input.BitangentWS,
        faceSign);
    float normalMipVariance = 0.0f;
    const float3 normalWS = SampleMaterialNormal(
        normalTexture,
        materialSampler,
        input.TexCoord,
        tangentBasis,
        objectRoughnessAoNormalAndObjectIndex.z,
        normalMipVariance);

    const float4 metallicRoughnessSample =
        metallicRoughnessTexture.Sample(materialSampler, input.TexCoord);
    const float metallic = SanitizeUnit(
        objectEmissiveAndMetallic.w * metallicRoughnessSample.b,
        0.0f);
    const float sampledRoughness =
        objectRoughnessAoNormalAndObjectIndex.x * metallicRoughnessSample.g;
    float roughness = isfinite(sampledRoughness)
        ? clamp(sampledRoughness, 0.045f, 1.0f)
        : 1.0f;
    roughness = ApplySpecularAntiAliasing(
        roughness,
        normalWS,
        normalMipVariance);
    const float aoSample = SanitizeUnit(
        occlusionTexture.Sample(materialSampler, input.TexCoord).r,
        1.0f);
    const float ao = lerp(
        1.0f,
        aoSample,
        SanitizeUnit(objectRoughnessAoNormalAndObjectIndex.y, 1.0f));
    const float3 viewWS = SafeNormalize(
        g_CameraPositionAndExposure.xyz - input.PositionWS);

    const float nDotV = saturate(dot(normalWS, viewWS));
    const float3 f0 = lerp(0.04f.xxx, baseColor, metallic);
    const float3 environmentFresnel = FresnelSchlickRoughness(nDotV, f0, roughness);
    const float3 environmentDiffuseWeight =
        (1.0f - environmentFresnel) * (1.0f - metallic);
    const float3 irradiance = g_IrradianceMap.SampleLevel(
        g_ShadowSampler, normalWS, 0.0f).rgb;
    const float3 reflectionWS = reflect(-viewWS, normalWS);
    const float3 prefiltered = g_PrefilterMap.SampleLevel(
        g_ShadowSampler,
        reflectionWS,
        roughness * g_IblSettings.y).rgb;
    const float2 brdf = g_BrdfLut.SampleLevel(
        g_ShadowSampler,
        float2(nDotV, roughness),
        0.0f).rg;
    const float3 diffuseIbl = environmentDiffuseWeight * irradiance *
        baseColor * ao * g_IblSettings.x;
    const float specularOcclusion = ComputeSpecularOcclusion(nDotV, ao, roughness);
    const float3 specularIbl = prefiltered *
        (f0 * brdf.x + brdf.y) * specularOcclusion * g_IblSettings.x;
    float3 directLighting = 0.0f;
    float directionalShadow = 1.0f;
    float3 shadowCascadeDebugColor = 1.0f;
    if (g_DirectionalDirectionAndIntensity.w > 0.0f)
    {
        const float3 lightWS = SafeNormalize(-g_DirectionalDirectionAndIntensity.xyz);
        const float3 radiance = g_DirectionalColor.rgb * g_DirectionalDirectionAndIntensity.w;
        directionalShadow = EvaluateDirectionalShadow(
            input.PositionWS,
            geometricNormal,
            lightWS,
            shadowCascadeDebugColor);
        directLighting += directionalShadow * EvaluateLight(
            baseColor, metallic, roughness, normalWS, viewWS, lightWS, radiance);
    }

    if (g_PointPositionAndIntensity.w > 0.0f && g_PointColorAndRadius.w > 0.0f)
    {
        const float3 toLight = g_PointPositionAndIntensity.xyz - input.PositionWS;
        const float distanceToLight = length(toLight);
        const float radius = g_PointColorAndRadius.w;
        const float attenuation = ComputePointLightAttenuation(
            distanceToLight,
            radius,
            g_DirectionalShadowDebugSettings.z);
        const float3 radiance = g_PointColorAndRadius.rgb *
            g_PointPositionAndIntensity.w * attenuation;
        directLighting += EvaluateLight(
            baseColor,
            metallic,
            roughness,
            normalWS,
            viewWS,
            SafeNormalize(toLight),
            radiance);
    }

    const float3 emissive =
        emissiveTexture.Sample(materialSampler, input.TexCoord).rgb *
        objectEmissiveAndMetallic.rgb;
    float3 color = directLighting + diffuseIbl + specularIbl + emissive;

    const uint pbrDebugMode = (uint)round(max(
        g_DirectionalShadowDebugSettings.y,
        0.0f));
    if (pbrDebugMode == c_PbrDebugBaseColor)
        color = baseColor;
    else if (pbrDebugMode == c_PbrDebugWorldNormal)
        color = normalWS * 0.5f + 0.5f;
    else if (pbrDebugMode == c_PbrDebugMetallic)
        color = metallic.xxx;
    else if (pbrDebugMode == c_PbrDebugRoughness)
        color = roughness.xxx;
    else if (pbrDebugMode == c_PbrDebugAo)
        color = ao.xxx;
    else if (pbrDebugMode == c_PbrDebugDirect)
        color = directLighting;
    else if (pbrDebugMode == c_PbrDebugDiffuseIbl)
        color = diffuseIbl;
    else if (pbrDebugMode == c_PbrDebugSpecularIbl)
        color = specularIbl;
    else if (pbrDebugMode == c_PbrDebugShadow)
        color = directionalShadow.xxx;

    // Cascade tint is an overlay, so it remains visible when combined with a
    // PBR component view (notably Shadow + cascade debug during capture).
    if (g_DirectionalShadowDebugSettings.x > 0.5f && g_ShadowSettings.x > 0.0f)
        color = lerp(color, shadowCascadeDebugColor, 0.65f);

    // Exposure, tone mapping and transfer-function encoding intentionally happen
    // in SceneToneMap.hlsl, once per output pixel.
    return float4(SanitizeHalfRadiance(color), alpha);
}
