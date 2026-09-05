#include "Include/Common.hlsli"
#include "Include/Skinning.hlsli"

#pragma htype vs VS
#pragma htype ps PS
#pragma hvariant SKINNED

struct ShadowDepthVaryings
{
    float4 PositionHS : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

namespace Properties
{
    Texture2D g_AlbedoMap : register(t0, space1);

    cbuffer ShadowMaterial : register(b3, space1)
    {
        float4 g_BaseColorFactor;
        float4 g_EmissiveFactor;
        float g_MetallicFactor;
        float g_RoughnessFactor;
        float g_AoStrength;
        float g_NormalStrength;
        float g_AlphaClip;
        float g_AlphaCutoff;
        float2 _ShadowMaterialPadding;
    }
}

#ifdef SKINNED
ShadowDepthVaryings VS(VSInputSkinned input, uint instanceId : SV_InstanceID)
{
    const float4x4 skinMatrix = ComputeSkinMatrix(input.BoneIDs, input.Weights, instanceId);
    const float3 skinnedPos = mul(skinMatrix, float4(input.pos, 1.0f)).xyz;

    ShadowDepthVaryings output;
    const float4x4 model = g_InstancingSettings.x != 0 ? g_InstanceModels[instanceId] : g_Model;
    output.PositionHS = mul(g_ViewProjection, mul(model, float4(skinnedPos, 1.0f)));
    output.TexCoord = input.texcoord;
    return output;
}
#else
ShadowDepthVaryings VS(VertexIn input, uint instanceId : SV_InstanceID)
{
    ShadowDepthVaryings output;
    const float4x4 model = g_InstancingSettings.x != 0 ? g_InstanceModels[instanceId] : g_Model;
    output.PositionHS = mul(g_ViewProjection, mul(model, float4(input.pos, 1.0f)));
    output.TexCoord = input.texcoord;
    return output;
}
#endif

void PS(ShadowDepthVaryings input)
{
    if (g_AlphaClip >= 0.5f)
    {
        const float alpha = Properties::g_AlbedoMap.Sample(g_LinearWrapSampler, input.TexCoord).a * g_BaseColorFactor.a;
        clip(alpha - g_AlphaCutoff);
    }
}
