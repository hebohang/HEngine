#include "Include/Common.hlsli"
#include "Include/Skinning.hlsli"

#pragma htype vs VS
#pragma htype ps PS
#pragma hvariant SKINNED

cbuffer OutlineSettings : register(b3, space3)
{
    float g_OutlineWidthPx;
    float3 g_OutlineColor;
    float2 g_InvViewportSize;
    float g_NormalOffsetWS;
    float g_DepthBias;
}

struct OutlineVSOut
{
    float4 PositionHS : SV_POSITION;
};

float4 ComputeOutlineClipPosition(float3 worldPos, float3 worldNormal)
{
    float4 clipPos = mul(g_ViewProjection, float4(worldPos, 1.0f));
    const float3 normalWS = SafeNormalize(worldNormal);
    const float4 clipProbe = mul(g_ViewProjection, float4(worldPos + normalWS * g_NormalOffsetWS, 1.0f));

    const float2 ndc = clipPos.xy / max(abs(clipPos.w), 1e-5f);
    const float2 probeNdc = clipProbe.xy / max(abs(clipProbe.w), 1e-5f);
    float2 ndcDir = probeNdc - ndc;

    const float2 ndcPerPixel = max(2.0f * g_InvViewportSize, float2(1e-6f, 1e-6f));
    const float dirLength = length(ndcDir / ndcPerPixel);
    if (dirLength < 1e-5f)
        ndcDir = float2(0.0f, 1.0f) * ndcPerPixel;
    else
        ndcDir = normalize(ndcDir / ndcPerPixel) * ndcPerPixel;

    clipPos.xy += ndcDir * g_OutlineWidthPx * clipPos.w;
    clipPos.z += g_DepthBias * clipPos.w;
    return clipPos;
}

#ifdef SKINNED
OutlineVSOut VS(VSInputSkinned input, uint instanceId : SV_InstanceID)
{
    OutlineVSOut output;
    const float4x4 skinMatrix = ComputeSkinMatrix(input.BoneIDs, input.Weights, instanceId);
    const float3 skinnedPos = mul(skinMatrix, float4(input.pos, 1.0f)).xyz;
    const float3 skinnedNormal = mul((float3x3)skinMatrix, input.normal);
    const float3 worldPos = mul(g_Model, float4(skinnedPos, 1.0f)).xyz;
    const float3 worldNormal = mul((float3x3)g_ModelInvTranspose, skinnedNormal);
    output.PositionHS = ComputeOutlineClipPosition(worldPos, worldNormal);
    return output;
}
#else
OutlineVSOut VS(VertexIn input)
{
    OutlineVSOut output;
    const float3 worldPos = mul(g_Model, float4(input.pos, 1.0f)).xyz;
    const float3 worldNormal = mul((float3x3)g_ModelInvTranspose, input.normal);
    output.PositionHS = ComputeOutlineClipPosition(worldPos, worldNormal);
    return output;
}
#endif

float4 PS(OutlineVSOut input) : SV_Target
{
    return float4(g_OutlineColor, 1.0f);
}
