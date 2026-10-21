#include "Shared/TextureSampling.hlsli"
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
#ifndef HE_MATERIAL_PASS_GBUFFER
#define HE_MATERIAL_PASS_GBUFFER 0
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
#ifndef HE_MATERIAL_HAS_VERTEX_OFFSET
#define HE_MATERIAL_HAS_VERTEX_OFFSET 0
#endif

// Surface Material ABI v2 follows the existing SceneRenderer register spaces:
// space0 view/frame, space1 material textures, space2 object/material data,
// space3 pass/environment/shadow, and space4 material samplers.
#include "BakedIrradianceTypes.hlsli"
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
    // x: current presentation time, y: previous presentation time (seconds).
    float4 g_HEMaterialTimeAndPadding;
    float4x4 g_HEPreviousViewProjection;
    float4x4 g_HEPreviousView;
    float4 g_HEFogColorDensity;
    float4 g_HEFogDistanceHeight;
    float4 g_HEFogDirectionalColorIntensity;
    float4 g_HEFogDirectionalParams;
    float4 g_HESecondaryFogColorDensity;
    float4 g_HESecondaryFogDistanceHeight;
    uint4 g_BakedIrradianceCount;
    HEBakedIrradianceVolume g_BakedIrradianceVolumes[16];
};

#if HE_MATERIAL_GPU_DRIVEN
struct HEGpuObjectData
{
    float4x4 World;
    float4x4 WorldInverseTranspose;
    uint4 MaterialObjectMeshIndices;
    float4x4 PreviousWorld;
    float4 StaticLightmapUvScaleBias;
    float4 StaticLightmapScale0;
    float4 StaticLightmapAdd0;
    float4 StaticLightmapScale1;
    float4 StaticLightmapAdd1;
    uint4 StaticLightmapDescriptor;
};
StructuredBuffer<HEGpuObjectData> g_HEGpuObjects : register(t0, space2);
// Invocation-local state makes the existing material property ABI identical
// for CPU and GPU instance paths, including pixel-stage property evaluation.
static float4x4 g_HEWorld;
static float4x4 g_HEWorldInverseTranspose;
static uint4 g_HEMaterialObjectMeshIndices;
static float4x4 g_HEPreviousWorld;
static float4 g_HEStaticLightmapUvScaleBias;
static float4 g_HEStaticLightmapScale0;
static float4 g_HEStaticLightmapAdd0;
static float4 g_HEStaticLightmapScale1;
static float4 g_HEStaticLightmapAdd1;
static uint4 g_HEStaticLightmapDescriptor;
void HE_LoadGpuObject(uint objectIndex)
{
    const HEGpuObjectData objectData = g_HEGpuObjects[objectIndex];
    g_HEWorld = objectData.World;
    g_HEWorldInverseTranspose = objectData.WorldInverseTranspose;
    g_HEMaterialObjectMeshIndices = objectData.MaterialObjectMeshIndices;
    g_HEPreviousWorld = objectData.PreviousWorld;
    g_HEStaticLightmapUvScaleBias = objectData.StaticLightmapUvScaleBias;
    g_HEStaticLightmapScale0 = objectData.StaticLightmapScale0;
    g_HEStaticLightmapAdd0 = objectData.StaticLightmapAdd0;
    g_HEStaticLightmapScale1 = objectData.StaticLightmapScale1;
    g_HEStaticLightmapAdd1 = objectData.StaticLightmapAdd1;
    g_HEStaticLightmapDescriptor = objectData.StaticLightmapDescriptor;
}
#else
cbuffer HEMaterialObjectConstants : register(b0, space2)
{
    float4x4 g_HEWorld;
    float4x4 g_HEWorldInverseTranspose;
    uint4 g_HEMaterialObjectMeshIndices;
    float4x4 g_HEPreviousWorld;
    float4 g_HEStaticLightmapUvScaleBias;
    float4 g_HEStaticLightmapScale0;
    float4 g_HEStaticLightmapAdd0;
    float4 g_HEStaticLightmapScale1;
    float4 g_HEStaticLightmapAdd1;
    uint4 g_HEStaticLightmapDescriptor;
};

#endif

struct HEGpuMaterialData
{
    float4 BaseColor;
    float4 EmissiveAndMetallic;
    float4 RoughnessAoNormalAndPadding;
    uint4 MaterialTextureIndices;
    uint4 EmissiveSamplerFlags;
    // x: program generation, y: parameter row offset, z: row count, w: ABI version.
    uint4 ProgramParameterData;
    HETextureSampling Sampling[5];
};

StructuredBuffer<HEGpuMaterialData> g_HEGpuMaterials : register(t2, space2);
#if HE_MATERIAL_SKINNED
StructuredBuffer<float4x4> g_HEBoneMatrices : register(t3, space2);
#if HE_MATERIAL_PASS_GBUFFER
StructuredBuffer<float4x4> g_HEPreviousBoneMatrices : register(t6, space2);
#endif
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
    float2 UV1;
    float4 Color;
    float3 PositionWS;
    float3 NormalWS;
    float3 TangentWS;
    float3 BitangentWS;
    float3 ViewWS;
    bool bFrontFace;
    // Monotonic presentation time in seconds, captured in the RenderWorld snapshot.
    float TimeSeconds;
};

float2 HE_SelectMaterialUV(uint textureUvSet, float2 uv0, float2 uv1)
{
    return textureUvSet == 1u ? uv1 : uv0;
}

// Standard custom-material texture semantics share the same persisted sampling
// settings as the legacy PBR path. Keep their UV transform in the renderer's
// per-material data so instances can change it without recompiling shaders.
float2 HE_StandardMaterialTextureUV(uint slot, float2 uv0, float2 uv1)
{
    return HE_TextureUv(g_HEGpuMaterials[g_HEMaterialObjectMeshIndices.x].Sampling[slot],
        uv0, uv1);
}

#include "Shared/NormalMapDecode.hlsli"

// The compiler supplies per-property flags from the texture parameter row.
// Token pasting intentionally precedes expansion of the texture property.
#define HE_TEXTURE_NORMAL_FLAGS(name) HE_TEXTURE_NORMAL_FLAGS_##name
#define HE_SAMPLE_NORMAL_MAP(name, samplerState, uv, scale, variance) \
    HE_DecodeNormalMap(name.Sample(samplerState, uv), scale, variance, HE_TEXTURE_NORMAL_FLAGS_##name)

struct HEMaterialSurface
{
    float3 BaseColor;
    float Metallic;
    // Dielectric specular level (0..1 maps to F0 0..0.08). Kept separate from
    // metallic so authored eye gloss does not turn the iris into a conductor.
    float SpecularLevel;
    float PerceptualRoughness;
    float AO;
    float3 NormalTS;
    uint NormalUvSet;
    float NormalMipVariance;
    float3 Emissive;
    float Opacity;
    float AlphaCutoff;
    // 0 standard, 2 skin, 3 hair, 4 cloth, 6 eye, 7 thin foliage transmission.
    uint CharacterModel;
    float3 CharacterTint;
    float CharacterStrength;
    float3 StrandDirectionWS;
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
    surface.SpecularLevel = 0.5f;
    surface.PerceptualRoughness = material.RoughnessAoNormalAndPadding.x;
    surface.AO = material.RoughnessAoNormalAndPadding.y;
    surface.NormalTS = float3(0.0f, 0.0f, 1.0f);
    surface.NormalUvSet = 0u;
    surface.NormalMipVariance = 0.0f;
    surface.Emissive = material.EmissiveAndMetallic.rgb;
    surface.Opacity = material.BaseColor.a;
    surface.AlphaCutoff = asfloat(material.EmissiveSamplerFlags.w);
    surface.CharacterModel = 0u;
    surface.CharacterTint = 0.0f;
    surface.CharacterStrength = 0.0f;
    surface.StrandDirectionWS = float3(0, 1, 0);
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

float3x3 HE_BuildWorldTbn(HEMaterialInput input, uint uvSet)
{
    if (uvSet != 1u)
        return HE_BuildWorldTbn(input);

    const float2 uvDx = ddx(input.UV1);
    const float2 uvDy = ddy(input.UV1);
    const float determinant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
    if (!isfinite(determinant) || abs(determinant) < 1e-8f)
        return HE_BuildWorldTbn(input);

    const float3 positionDx = ddx(input.PositionWS);
    const float3 positionDy = ddy(input.PositionWS);
    const float3 sourceNormal = HE_SafeNormalize(input.NormalWS);
    const float3 tangentCandidate =
        (positionDx * uvDy.y - positionDy * uvDx.y) / determinant;
    const float3 bitangentCandidate =
        (positionDy * uvDx.x - positionDx * uvDy.x) / determinant;
    if (!all(isfinite(tangentCandidate)) ||
        !all(isfinite(bitangentCandidate)))
        return HE_BuildWorldTbn(input);
    const float3 orthogonalTangent = tangentCandidate -
        sourceNormal * dot(sourceNormal, tangentCandidate);
    if (dot(orthogonalTangent, orthogonalTangent) < 1e-8f)
        return HE_BuildWorldTbn(input);

    const float3 tangent = HE_SafeNormalize(orthogonalTangent);
    const float handedness = dot(cross(sourceNormal, tangent),
        bitangentCandidate) < 0.0f ? -1.0f : 1.0f;
    const float3 normal = sourceNormal * (input.bFrontFace ? 1.0f : -1.0f);
    const float3 bitangent = HE_SafeNormalize(cross(normal, tangent)) * handedness;
    return float3x3(tangent, bitangent, normal);
}

float3 HE_ResolveMaterialNormal(HEMaterialInput input, float3 normalTS,
    uint normalUvSet = 0u)
{
    normalTS = all(isfinite(normalTS))
        ? HE_SafeNormalize(normalTS)
        : float3(0.0f, 0.0f, 1.0f);
    const float3x3 tangentBasis = HE_BuildWorldTbn(input, normalUvSet);
    return HE_SafeNormalize(
        normalTS.x * tangentBasis[0] +
        normalTS.y * tangentBasis[1] +
        normalTS.z * tangentBasis[2]);
}

float HE_ApplySpecularAntiAliasing(float roughness, float3 normalWS,
    float normalMipVariance = 0.0f)
{
    const float3 dndx = ddx(normalWS);
    const float3 dndy = ddy(normalWS);
    const float normalVariance = all(isfinite(dndx)) && all(isfinite(dndy))
        ? min((dot(dndx, dndx) + dot(dndy, dndy)) / 12.0f, 0.35f)
        : 0.35f;
    const float minimumAlpha = 0.08f * 0.08f;
    const float materialAlpha = max(roughness * roughness, minimumAlpha);
    const float mipVariance = isfinite(normalMipVariance)
        ? clamp(normalMipVariance, 0.0f, HE_MAX_NORMAL_MIP_VARIANCE) : 0.0f;
    return sqrt(saturate(materialAlpha + max(normalVariance, 0.0f) + mipVariance));
}

#if HE_MATERIAL_PASS_FORWARD
#define g_CameraPositionAndExposure g_HECameraPositionAndExposure
#define g_LightViewProjection g_HELightViewProjection
#define g_AdditionalLightViewProjections g_HEAdditionalLightViewProjections
#define g_ShadowCameraView g_HEShadowCameraView
#define g_DirectionalShadowCascadeSettings g_HEDirectionalShadowCascadeSettings
#define g_DirectionalShadowCascadeSplits g_HEDirectionalShadowCascadeSplits
#define g_DirectionalShadowDebugSettings g_HEDirectionalShadowDebugSettings
#define g_ShadowSettings g_HEShadowSettings
#define g_DirectionalShadow g_HEDirectionalShadow
#define g_ShadowSampler g_HEEnvironmentSampler
#define g_IrradianceMap g_HEIrradianceMap
#define g_PrefilterMap g_HEPrefilterMap
#define g_BrdfLut g_HEBrdfLut
#define g_IblSettings g_HEIblSettings
#define g_DirectionalDirectionAndIntensity g_HEDirectionalDirectionAndIntensity
#define g_DirectionalColor g_HEDirectionalColor
#define g_PointPositionAndIntensity g_HEPointPositionAndIntensity
#define g_PointColorAndRadius g_HEPointColorAndRadius
#define g_ViewProjection g_HEViewProjection
#define g_FogColorDensity g_HEFogColorDensity
#define g_FogDistanceHeight g_HEFogDistanceHeight
#define g_FogDirectionalColorIntensity g_HEFogDirectionalColorIntensity
#define g_FogDirectionalParams g_HEFogDirectionalParams
#define g_SecondaryFogColorDensity g_HESecondaryFogColorDensity
#define g_SecondaryFogDistanceHeight g_HESecondaryFogDistanceHeight
#include "RasterSurfaceLighting.hlsli"
#undef g_FogColorDensity
#undef g_FogDistanceHeight
#undef g_FogDirectionalColorIntensity
#undef g_FogDirectionalParams
#undef g_SecondaryFogColorDensity
#undef g_SecondaryFogDistanceHeight
#undef g_ViewProjection
#undef g_CameraPositionAndExposure
#undef g_LightViewProjection
#undef g_AdditionalLightViewProjections
#undef g_ShadowCameraView
#undef g_DirectionalShadowCascadeSettings
#undef g_DirectionalShadowCascadeSplits
#undef g_DirectionalShadowDebugSettings
#undef g_ShadowSettings
#undef g_DirectionalShadow
#undef g_ShadowSampler
#undef g_IrradianceMap
#undef g_PrefilterMap
#undef g_BrdfLut
#undef g_IblSettings
#undef g_DirectionalDirectionAndIntensity
#undef g_DirectionalColor
#undef g_PointPositionAndIntensity
#undef g_PointColorAndRadius

float3 HE_ShadeMaterial(HEMaterialInput input, HEMaterialSurface surface)
{
    surface.BaseColor = saturate(surface.BaseColor);
    surface.Metallic = HE_SanitizeUnit(surface.Metallic, 0.0f);
    surface.PerceptualRoughness = isfinite(surface.PerceptualRoughness)
        ? clamp(surface.PerceptualRoughness, 0.045f, 1.0f) : 1.0f;
    surface.AO = HE_SanitizeUnit(surface.AO, 1.0f);
    surface.Emissive = HE_SanitizeHdr(surface.Emissive);
#if HE_MATERIAL_SHADING_UNLIT
    return HE_SanitizeHdr(surface.BaseColor + surface.Emissive);
#else
    const float3 normalWS = HE_ResolveMaterialNormal(input, surface.NormalTS,
        surface.NormalUvSet);
    surface.PerceptualRoughness = HE_ApplySpecularAntiAliasing(
        surface.PerceptualRoughness, normalWS, surface.NormalMipVariance);
    if (surface.CharacterModel == 3u)
        surface.PerceptualRoughness = HE_ApplySpecularAntiAliasing(
            surface.PerceptualRoughness, HE_SafeNormalize(surface.StrandDirectionWS));
    const float3 geometricNormalWS = HE_SafeNormalize(input.NormalWS) *
        (input.bFrontFace ? 1.0f : -1.0f);
    return ShadeSceneSurface(input.PositionWS, surface.BaseColor,
        surface.Metallic, surface.PerceptualRoughness, surface.AO,
        normalWS, geometricNormalWS, surface.Emissive,
        !HE_MATERIAL_BLEND_TRANSPARENT, surface.CharacterModel,
        surface.CharacterTint * saturate(surface.CharacterStrength),
        HE_SafeNormalize(surface.StrandDirectionWS), saturate(surface.CharacterStrength),
        HE_SanitizeUnit(surface.SpecularLevel, 0.5f));
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
