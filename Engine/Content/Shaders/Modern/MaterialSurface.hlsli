#ifndef HE_MATERIAL_SURFACE_HLSLI
#define HE_MATERIAL_SURFACE_HLSLI

#include "MaterialSurfaceCommon.hlsli"

// The user material definition supplies this function after including this file.
void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface);

struct HEMaterialVertexInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD;
    float3 Tangent : TANGENT;
    float3 Bitangent : BINORMAL;
#if HE_MATERIAL_SKINNED
    int4 BoneIds : BONEIDS;
    float4 BoneWeights : BONEWEIGHTS;
#endif
};

struct HEMaterialVaryings
{
    float4 PositionCS : SV_Position;
    float3 PositionWS : POSITION0;
    float3 NormalWS : NORMAL0;
    float3 TangentWS : TANGENT0;
    float3 BitangentWS : BINORMAL0;
    float2 UV0 : TEXCOORD0;
};

HEMaterialVaryings HEMaterialVS(
    HEMaterialVertexInput input,
    uint instanceId : SV_InstanceID)
{
    float4 positionLS = float4(input.Position, 1.0f);
    float3 normalLS = input.Normal;
    float3 tangentLS = input.Tangent;
    float3 bitangentLS = input.Bitangent;
#if HE_MATERIAL_SKINNED
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
        const float4x4 bone =
            g_HEBoneMatrices[g_HEMaterialObjectMeshIndices.w + boneId];
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

    const float4 positionWS = mul(g_HEWorld, positionLS);
    float4 rasterPositionWS = positionWS;
#if HE_MATERIAL_PASS_OUTLINE
    rasterPositionWS.xyz += HE_SafeNormalize(
        mul((float3x3)g_HEWorldInverseTranspose, normalLS)) * 0.025f;
#endif
    HEMaterialVaryings output;
#if HE_MATERIAL_PASS_SHADOW
    output.PositionCS = mul(
        HE_GetShadowPassMatrix(g_HEShadowCascadeIndex), rasterPositionWS);
#else
    output.PositionCS = mul(g_HEViewProjection, rasterPositionWS);
#endif
    output.PositionWS = positionWS.xyz;
    output.NormalWS = HE_SafeNormalize(
        mul((float3x3)g_HEWorldInverseTranspose, normalLS));
    output.TangentWS = HE_SafeNormalize(mul((float3x3)g_HEWorld, tangentLS));
    output.BitangentWS = HE_SafeNormalize(mul((float3x3)g_HEWorld, bitangentLS));
    output.UV0 = input.TexCoord;
    return output;
}

HEMaterialInput HE_BuildMaterialInput(
    HEMaterialVaryings input,
    bool bIsFrontFace)
{
    HEMaterialInput materialInput;
    materialInput.UV0 = input.UV0;
    materialInput.PositionWS = input.PositionWS;
    materialInput.NormalWS = input.NormalWS;
    materialInput.TangentWS = input.TangentWS;
    materialInput.BitangentWS = input.BitangentWS;
    materialInput.ViewWS = HE_SafeNormalize(
        g_HECameraPositionAndExposure.xyz - input.PositionWS);
    materialInput.bFrontFace = bIsFrontFace;
    materialInput.TimeSeconds = g_HEMaterialTimeAndPadding.x;
    return materialInput;
}

#if HE_MATERIAL_PASS_FORWARD
float4 HEMaterialPS(
    HEMaterialVaryings input,
    bool bIsFrontFace : SV_IsFrontFace) : SV_Target0
{
    const HEMaterialInput materialInput =
        HE_BuildMaterialInput(input, bIsFrontFace);
    HEMaterialSurface surface = HE_DefaultMaterialSurface();
    EvaluateMaterial(materialInput, surface);
    surface.Opacity = HE_SanitizeUnit(surface.Opacity, 1.0f);
    surface.AlphaCutoff = HE_SanitizeUnit(surface.AlphaCutoff, 0.5f);
#if HE_MATERIAL_BLEND_MASK
    clip(surface.Opacity - surface.AlphaCutoff);
#endif
    const float outputAlpha = HE_MATERIAL_BLEND_TRANSPARENT
        ? surface.Opacity
        : 1.0f;
    // SceneRenderer's tone-map pass owns exposure and display encoding.
    return float4(HE_ShadeMaterial(materialInput, surface), outputAlpha);
}
#elif HE_MATERIAL_PASS_OUTLINE
float4 HEMaterialPS(
    HEMaterialVaryings input,
    bool bIsFrontFace : SV_IsFrontFace) : SV_Target0
{
    const HEMaterialInput materialInput =
        HE_BuildMaterialInput(input, bIsFrontFace);
    HEMaterialSurface surface = HE_DefaultMaterialSurface();
    EvaluateMaterial(materialInput, surface);
    surface.Opacity = HE_SanitizeUnit(surface.Opacity, 1.0f);
    surface.AlphaCutoff = HE_SanitizeUnit(surface.AlphaCutoff, 0.5f);
    clip(surface.Opacity - surface.AlphaCutoff);
    return float4(1.0f, 0.22f, 0.02f, 1.0f);
}
#else
void HEMaterialPS(
    HEMaterialVaryings input,
    bool bIsFrontFace : SV_IsFrontFace)
{
    const HEMaterialInput materialInput =
        HE_BuildMaterialInput(input, bIsFrontFace);
    HEMaterialSurface surface = HE_DefaultMaterialSurface();
    EvaluateMaterial(materialInput, surface);
    surface.Opacity = HE_SanitizeUnit(surface.Opacity, 1.0f);
    surface.AlphaCutoff = HE_SanitizeUnit(surface.AlphaCutoff, 0.5f);
    // Mask Forward, Depth, and Shadow all execute the same material opacity path.
    clip(surface.Opacity - surface.AlphaCutoff);
}
#endif

#endif
