#include "Shared/TextureSampling.hlsli"
#pragma htype vs VS
#pragma htype ps PS
#pragma hvariant GPU_DRIVEN
#pragma hvariant SKINNED

#include "MaterialDescriptorAccess.hlsli"
#include "Shared/BackendPushConstants.hlsli"

// Depth prepass binding contract mirrors SceneOpaque so MASK geometry uses the
// same tier-selected base-color alpha and cutoff as the forward pass.
cbuffer FrameConstants : register(b0, space0)
{
    float4x4 g_ViewProjection;
};

cbuffer ObjectConstants : register(b0, space2)
{
    float4x4 g_World;
    float4x4 g_WorldInverseTranspose;
    uint4 g_MaterialObjectMeshIndices;
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
#endif
#if GPU_DRIVEN
StructuredBuffer<GpuObjectData> g_GpuObjects : register(t0, space2);
#endif

struct VertexInput
{
    float3 Position : POSITION;
    float2 TexCoord : TEXCOORD;
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

struct DepthVaryings
{
    float4 PositionCS : SV_Position;
    float2 TexCoord : TEXCOORD0;
    float2 TexCoord1 : TEXCOORD6;
    float4 Color : COLOR0;
#if GPU_DRIVEN
    nointerpolation uint ObjectIndex : TEXCOORD1;
#endif
};

DepthVaryings VS(VertexInput input, uint instanceId : SV_InstanceID)
{
#if GPU_DRIVEN
    const uint objectIndex = input.ObjectIndex;
    const GpuObjectData gpuObject = g_GpuObjects[objectIndex];
    const float4x4 world = gpuObject.World;
#else
    const float4x4 world = g_World;
#endif
    float4 positionLS = float4(input.Position, 1.0f);
#if SKINNED
    float4 skinnedPosition = 0.0f;
    float totalWeight = 0.0f;
    [unroll]
    for (uint influence = 0; influence < 16; ++influence)
    {
        const int boneId = input.BoneIds[influence / 4][influence % 4];
        const float weight = input.BoneWeights[influence / 4][influence % 4];
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
    DepthVaryings output;
    const float4 positionWS = mul(world, positionLS);
    output.PositionCS = mul(g_ViewProjection, positionWS);
    output.TexCoord = input.TexCoord;
    output.TexCoord1 = input.TexCoord1;
    output.Color = input.Color;
#if GPU_DRIVEN
    output.ObjectIndex = objectIndex;
#endif
    return output;
}

void PS(DepthVaryings input)
{
#if GPU_DRIVEN
    const uint materialIndex =
        g_GpuObjects[input.ObjectIndex].MaterialObjectMeshIndices.x;
#else
    const uint materialIndex = g_MaterialObjectMeshIndices.x;
#endif
    const GpuMaterialData material = g_GpuMaterials[materialIndex];
    const uint materialFlags = material.EmissiveSamplerFlags.z;
    if ((materialFlags & 1u) == 0u)
        return;

    Texture2D<float4> baseColorTexture =
        HE_MATERIAL_TEXTURE(material.MaterialTextureIndices.x);
    SamplerState materialSampler =
        HE_MATERIAL_SAMPLER(material.EmissiveSamplerFlags.y);
    const float alpha = HE_SampleTexture(baseColorTexture, materialSampler, material.Sampling[0], input.TexCoord, input.TexCoord1).a *
        material.BaseColor.a * input.Color.a;
    clip(alpha - asfloat(material.EmissiveSamplerFlags.w));
}
