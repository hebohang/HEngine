#include "Shared/TextureSampling.hlsli"
#pragma htype vs VS
#pragma htype ps PS
#pragma hvariant GPU_DRIVEN
#pragma hvariant SKINNED
#pragma hvariant GBUFFER
#pragma hvariant GBUFFER_SKINNED
#pragma hvariant GBUFFER_GPU_DRIVEN
#pragma hvariant GUIDES
#pragma hvariant GUIDES_SKINNED
#pragma hvariant GUIDES_GPU_DRIVEN

#if GUIDES || GUIDES_SKINNED || GUIDES_GPU_DRIVEN
#define GBUFFER 1
#define HE_RASTER_GUIDES_ONLY 1
#endif
#if GUIDES_SKINNED
#define SKINNED 1
#endif
#if GUIDES_GPU_DRIVEN
#define GPU_DRIVEN 1
#endif

#if GBUFFER_SKINNED
#define GBUFFER 1
#define SKINNED 1
#endif
#if GBUFFER_GPU_DRIVEN
#define GBUFFER 1
#define GPU_DRIVEN 1
#endif

#include "MaterialDescriptorAccess.hlsli"
#include "Shared/BackendPushConstants.hlsli"
#include "RasterSurfaceData.hlsli"

// Tiered SceneRenderer binding contract:
// space0: per-view data, space1: bounded tables or global descriptor heaps,
// space2: per-object/material constants. Pixel output stays linear HDR.
#include "BakedIrradianceTypes.hlsli"
cbuffer FrameConstants : register(b0, space0)
{
    float4x4 g_ViewProjection;
    float4x4 g_LightViewProjection;
    float4 g_CameraPositionAndExposure;
    float4 g_DirectionalDirectionAndIntensity;
    float4 g_DirectionalColor;
    float4 g_PointPositionAndIntensity;
    float4 g_PointColorAndRadius;
    // x: strength, y: constant bias, z: normal bias.
    // w >= 0: legacy radius; w < 0: continuous radius encoded as -(radius+1).
    float4 g_ShadowSettings;
    // x: IBL intensity, y: prefilter max mip, z: resources ready.
    float4 g_IblSettings;
    float4x4 g_InverseViewProjection;
    float4x4 g_ShadowCameraView;
    float4x4 g_AdditionalLightViewProjections[3];
    float4 g_DirectionalShadowCascadeSplits;
    // x: cascade count, y: map size, z: blend fraction, w: far fade fraction.
    float4 g_DirectionalShadowCascadeSettings;
    // x: cascade debug, y: PBR debug mode, z: reserved,
    // w: sanitized camera near plane used by the first cascade blend span.
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

cbuffer ObjectConstants : register(b0, space2)
{
    float4x4 g_World;
    float4x4 g_WorldInverseTranspose;
    // x: GPU material, y: stable object, z: primitive base for this section,
    // w: current/previous bone-palette offset.
    uint4 g_MaterialObjectMeshIndices;
    float4x4 g_PreviousWorld;
    float4 g_StaticLightmapUvScaleBias;
    float4 g_StaticLightmapScale0;
    float4 g_StaticLightmapAdd0;
    float4 g_StaticLightmapScale1;
    float4 g_StaticLightmapAdd1;
    uint4 g_StaticLightmapDescriptor;
};

struct GpuObjectData
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

struct GpuMaterialData
{
    float4 BaseColor;
    float4 EmissiveAndMetallic;
    float4 RoughnessAoNormalAndPadding;
    uint4 MaterialTextureIndices;
    uint4 EmissiveSamplerFlags;
    uint4 ProgramParameterData;
    HETextureSampling Sampling[5];
};

StructuredBuffer<GpuMaterialData> g_GpuMaterials : register(t2, space2);
#if SKINNED
StructuredBuffer<float4x4> g_BoneMatrices : register(t3, space2);
#if GBUFFER
StructuredBuffer<float4x4> g_PreviousBoneMatrices : register(t6, space2);
#endif
#endif

#if GPU_DRIVEN
StructuredBuffer<GpuObjectData> g_GpuObjects : register(t0, space2);
#endif

struct VertexInput
{
    float3 Position : POSITION;
    float3 PreviousPosition : PREVIOUS_POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD;
    float3 Tangent : TANGENT;
    float3 Bitangent : BINORMAL;
#if SKINNED
    int4 BoneIds[4] : BONEIDS;
    float4 BoneWeights[4] : BONEWEIGHTS;
#endif
    float2 TexCoord1 : TEXCOORDONE;
    float4 Color : COLOR;
#if GPU_DRIVEN
    uint ObjectIndex : INPUT_OBJECT_ID;
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
    float2 TexCoord1 : TEXCOORD6;
    float4 Color : COLOR0;
#if GBUFFER
    float4 CurrentClip : TEXCOORD2;
    float4 PreviousClip : TEXCOORD3;
    float PreviousViewDepth : TEXCOORD4;
#endif
#if GPU_DRIVEN
    nointerpolation uint ObjectIndex : TEXCOORD1;
#endif
};

SurfaceVaryings VS(VertexInput input, uint instanceId : SV_InstanceID)
{
#if GPU_DRIVEN
    // IA applies StartInstanceLocation to this per-instance stream on both
    // backends. SV_InstanceID itself does not include the base instance.
    const uint objectIndex = input.ObjectIndex;
    const GpuObjectData gpuObject = g_GpuObjects[objectIndex];
    const float4x4 world = gpuObject.World;
    const float4x4 worldInverseTranspose = gpuObject.WorldInverseTranspose;
    const float4x4 previousWorld = gpuObject.PreviousWorld;
#else
    const float4x4 world = g_World;
    const float4x4 worldInverseTranspose = g_WorldInverseTranspose;
    const float4x4 previousWorld = g_PreviousWorld;
#endif
    SurfaceVaryings output;
    float4 positionLS = float4(input.Position, 1.0f);
#if GBUFFER
    float4 previousPositionLS = float4(input.PreviousPosition, 1.0f);
#endif
    float3 normalLS = input.Normal;
    float3 tangentLS = input.Tangent;
    float3 bitangentLS = input.Bitangent;
#if SKINNED
    float4 skinnedPosition = 0.0f;
#if GBUFFER
    float4 previousSkinnedPosition = 0.0f;
#endif
    float3 skinnedNormal = 0.0f;
    float3 skinnedTangent = 0.0f;
    float3 skinnedBitangent = 0.0f;
    float totalWeight = 0.0f;
    [unroll]
    for (uint influence = 0; influence < 16; ++influence)
    {
        const int boneId = input.BoneIds[influence / 4][influence % 4];
        const float weight = input.BoneWeights[influence / 4][influence % 4];
        if (boneId < 0 || weight <= 0.0f)
            continue;
        const float4x4 bone = g_BoneMatrices[g_MaterialObjectMeshIndices.w + boneId];
        skinnedPosition += mul(bone, positionLS) * weight;
#if GBUFFER
        previousSkinnedPosition += mul(
            g_PreviousBoneMatrices[g_MaterialObjectMeshIndices.w + boneId],
            previousPositionLS) * weight;
#endif
        skinnedNormal += mul((float3x3)bone, normalLS) * weight;
        skinnedTangent += mul((float3x3)bone, tangentLS) * weight;
        skinnedBitangent += mul((float3x3)bone, bitangentLS) * weight;
        totalWeight += weight;
    }
    if (totalWeight > 0.0f)
    {
        positionLS = skinnedPosition / totalWeight;
#if GBUFFER
        previousPositionLS = previousSkinnedPosition / totalWeight;
#endif
        normalLS = skinnedNormal / totalWeight;
        tangentLS = skinnedTangent / totalWeight;
        bitangentLS = skinnedBitangent / totalWeight;
    }
#endif
    const float4 positionWS = mul(world, positionLS);
    output.PositionCS = mul(g_ViewProjection, positionWS);
#if GBUFFER
    output.CurrentClip = output.PositionCS;
    const float4 previousPositionWS = mul(previousWorld, previousPositionLS);
    output.PreviousClip = mul(g_PreviousViewProjection, previousPositionWS);
    // Interpolate signed depth before evaluating visibility in the pixel stage.
    output.PreviousViewDepth = -mul(g_PreviousView, previousPositionWS).z;
#endif
    output.PositionWS = positionWS.xyz;
    output.NormalWS = normalize(mul((float3x3)worldInverseTranspose, normalLS));
    output.TangentWS = normalize(mul((float3x3)world, tangentLS));
    output.BitangentWS = normalize(mul((float3x3)world, bitangentLS));
    output.TexCoord = input.TexCoord;
    output.TexCoord1 = input.TexCoord1;
    output.Color = input.Color;
#if GPU_DRIVEN
    output.ObjectIndex = objectIndex;
#endif
    return output;
}

#include "RasterSurfaceLighting.hlsli"
#include "StaticLightmapDirectionalAtlas.hlsli"

#if GBUFFER
HERasterSurfaceOutputs PS(SurfaceVaryings input, bool bIsFrontFace : SV_IsFrontFace,
    uint primitiveId : SV_PrimitiveID)
#else
float4 PS(SurfaceVaryings input, bool bIsFrontFace : SV_IsFrontFace) : SV_Target0
#endif
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
    const float4 baseSample = HE_SampleTexture(baseColorTexture, materialSampler, material.Sampling[0], input.TexCoord, input.TexCoord1);
    const uint materialFlags = emissiveSamplerFlags.z;
    const float alpha = SanitizeUnit(baseSample.a * objectBaseColor.a * input.Color.a, 0.0f);
    if ((materialFlags & 1u) != 0u)
        clip(alpha - asfloat(emissiveSamplerFlags.w));
    const float3 baseColor = baseSample.rgb * objectBaseColor.rgb * input.Color.rgb;
    const float faceSign = bIsFrontFace ? 1.0f : -1.0f;
    const float3 geometricNormal = SafeNormalize(input.NormalWS) * faceSign;
    float3x3 tangentBasis = BuildWorldTbn(
        input.NormalWS,
        input.TangentWS,
        input.BitangentWS,
        faceSign);
    float normalMipVariance = 0.0f;
    const HETextureSampling normalSampling = material.Sampling[1];
    if (normalSampling.Options.x != 0 || any(normalSampling.UvRow0.xyz != float3(1,0,0)) || any(normalSampling.UvRow1.xyz != float3(0,1,0)))
    {
        const float2 normalUv = HE_TextureUv(normalSampling, input.TexCoord, input.TexCoord1);
        const float3 dx = ddx(input.PositionWS), dy = ddy(input.PositionWS);
        const float2 du = ddx(normalUv), dv = ddy(normalUv);
        const float determinant = du.x * dv.y - du.y * dv.x;
        if (abs(determinant) > 1e-10)
        {
            const float3 tangent = (dx * dv.y - dy * du.y) / determinant;
            const float3 bitangent = (dy * du.x - dx * dv.x) / determinant;
            tangentBasis = BuildWorldTbn(input.NormalWS, tangent, bitangent, faceSign);
        }
    }
    const float3 tangentNormal = HE_DecodeNormalMap(
        HE_SampleTexture(normalTexture, materialSampler, normalSampling, input.TexCoord, input.TexCoord1),
        objectRoughnessAoNormalAndObjectIndex.z, normalMipVariance,
        (material.EmissiveSamplerFlags.z >> 3u) & 3u);
    const float3 normalWS = SafeNormalize(mul(tangentNormal, tangentBasis));

    const float4 metallicRoughnessSample =
        HE_SampleTexture(metallicRoughnessTexture, materialSampler, material.Sampling[2], input.TexCoord, input.TexCoord1);
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
        HE_SampleTexture(occlusionTexture, materialSampler, material.Sampling[3], input.TexCoord, input.TexCoord1).r,
        1.0f);
    const float ao = lerp(
        1.0f,
        aoSample,
        SanitizeUnit(objectRoughnessAoNormalAndObjectIndex.y, 1.0f));
    const float3 emissive =
        HE_SampleTexture(emissiveTexture, materialSampler, material.Sampling[4], input.TexCoord, input.TexCoord1).rgb *
        objectEmissiveAndMetallic.rgb;
#if GBUFFER
    HERasterSurfaceOutputs output = HE_EncodeRasterSurface(baseColor, metallic, normalWS, roughness, ao,
        emissive, false, input.CurrentClip, input.PreviousClip, input.PreviousViewDepth,
#if GPU_DRIVEN
        gpuObject.MaterialObjectMeshIndices.y,
        gpuObject.MaterialObjectMeshIndices.z + primitiveId);
#else
        g_MaterialObjectMeshIndices.y,
        g_MaterialObjectMeshIndices.z + primitiveId);
#endif
#if HE_HEAP_DIRECT && !HE_RASTER_GUIDES_ONLY
#if GPU_DRIVEN
    const uint staticLightmapIndex = gpuObject.StaticLightmapDescriptor.x;
    const uint staticLightmapEncoding = gpuObject.StaticLightmapDescriptor.z;
    const float staticLightmapBlend =
        gpuObject.StaticLightmapDescriptor.y / 65535.0f;
    const float4 lightmapUvScaleBias = gpuObject.StaticLightmapUvScaleBias;
    const float4 lightmapScale0 = gpuObject.StaticLightmapScale0;
    const float4 lightmapAdd0 = gpuObject.StaticLightmapAdd0;
    const float4 lightmapScale1 = gpuObject.StaticLightmapScale1;
    const float4 lightmapAdd1 = gpuObject.StaticLightmapAdd1;
#else
    const uint staticLightmapIndex = g_StaticLightmapDescriptor.x;
    const uint staticLightmapEncoding = g_StaticLightmapDescriptor.z;
    const float staticLightmapBlend = g_StaticLightmapDescriptor.y / 65535.0f;
    const float4 lightmapUvScaleBias = g_StaticLightmapUvScaleBias;
    const float4 lightmapScale0 = g_StaticLightmapScale0;
    const float4 lightmapAdd0 = g_StaticLightmapAdd0;
    const float4 lightmapScale1 = g_StaticLightmapScale1;
    const float4 lightmapAdd1 = g_StaticLightmapAdd1;
#endif
    if (staticLightmapIndex != 0xffffffffu)
    {
        Texture2D<float4> lightmapTexture = HE_MATERIAL_TEXTURE(staticLightmapIndex);
        const float3 irradiance = HE_DecodeDirectionalAtlasLightmap(lightmapTexture,
            materialSampler, input.TexCoord1, normalWS, lightmapUvScaleBias,
            lightmapScale0, lightmapAdd0, lightmapScale1, lightmapAdd1, staticLightmapEncoding);
        output.EmissiveModel = float4(emissive + irradiance *
            baseColor * (1.0f - metallic) / 3.14159265f *
            staticLightmapBlend, 2.0f + staticLightmapBlend * 0.25f);
    }
#endif
    return output;
#else
    return float4(ShadeSceneSurface(input.PositionWS, baseColor, metallic,
        roughness, ao, normalWS, geometricNormal, emissive,
        (materialFlags & 2u) == 0u), alpha);
#endif
}
