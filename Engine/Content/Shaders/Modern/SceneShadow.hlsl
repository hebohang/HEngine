#pragma htype vs VS
#pragma htype ps PS
#pragma hvariant SKINNED

#include "MaterialDescriptorAccess.hlsli"
#include "Shared/BackendPushConstants.hlsli"

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
    // x: cascade debug, y: PBR debug mode, z: first point falloff exponent,
    // w: sanitized camera near plane used by the first cascade blend span.
    float4 g_DirectionalShadowDebugSettings;
};

cbuffer ObjectConstants : register(b0, space2)
{
    float4x4 g_World;
    float4x4 g_WorldInverseTranspose;
    uint4 g_MaterialObjectMeshIndices;
};

struct ShadowPassConstants
{
    uint CascadeIndex;
    uint3 Padding;
};
HE_DECLARE_PUSH_CONSTANTS(ShadowPassConstants, g_ShadowPassConstants, space3);

#define g_ShadowCascadeIndex g_ShadowPassConstants.CascadeIndex

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

struct VertexInput
{
    float3 Position : POSITION;
    float2 TexCoord : TEXCOORD;
#if SKINNED
    int4 BoneIds : BONEIDS;
    float4 BoneWeights : BONEWEIGHTS;
#endif
};

struct ShadowVaryings
{
    float4 PositionCS : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

float4x4 GetDirectionalShadowMatrix(uint cascadeIndex)
{
    if (cascadeIndex == 0u)
        return g_LightViewProjection;
    return g_AdditionalLightViewProjections[min(cascadeIndex - 1u, 2u)];
}

ShadowVaryings VS(VertexInput input)
{
    float4 positionLS = float4(input.Position, 1.0f);
#if SKINNED
    float4 skinnedPosition = 0.0f;
    float totalWeight = 0.0f;
    [unroll]
    for (uint influence = 0; influence < 4; ++influence)
    {
        const int boneId = input.BoneIds[influence];
        const float weight = input.BoneWeights[influence];
        if (boneId < 0 || weight <= 0.0f)
            continue;
        skinnedPosition += mul(
            g_BoneMatrices[g_MaterialObjectMeshIndices.w + boneId],
            positionLS) * weight;
        totalWeight += weight;
    }
    if (totalWeight > 0.0f)
        positionLS = skinnedPosition / totalWeight;
#endif
    ShadowVaryings output;
    output.PositionCS = mul(
        GetDirectionalShadowMatrix(g_ShadowCascadeIndex),
        mul(g_World, positionLS));
    output.TexCoord = input.TexCoord;
    return output;
}

void PS(ShadowVaryings input)
{
    const GpuMaterialData material =
        g_GpuMaterials[g_MaterialObjectMeshIndices.x];
    const uint materialFlags = material.EmissiveSamplerFlags.z;
    if ((materialFlags & 1u) == 0u)
        return;

    Texture2D<float4> baseColorTexture =
        HE_MATERIAL_TEXTURE(material.MaterialTextureIndices.x);
    SamplerState materialSampler =
        HE_MATERIAL_SAMPLER(material.EmissiveSamplerFlags.y);
    const float alpha = baseColorTexture.Sample(materialSampler, input.TexCoord).a *
        material.BaseColor.a;
    clip(alpha - asfloat(material.EmissiveSamplerFlags.w));
}
