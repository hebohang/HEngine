#include "Include/Common.hlsli"
#include "Include/PBR/Sampling.hlsli"

#pragma htype vs VS
#pragma htype ps PS

TextureCube g_BackgroundEnvironmentMap : register(t4, space3);

struct SkyboxVaryings
{
    float4 PositionHS : SV_POSITION;
    float3 DirectionWS : TEXCOORD0;
};

SkyboxVaryings VS(VertexIn input)
{
    SkyboxVaryings output;
    output.DirectionWS = input.pos;
    output.PositionHS = mul(g_ViewProjection, float4(input.pos, 1.0f));
    output.PositionHS.z = output.PositionHS.w;
    return output;
}

float4 PS(SkyboxVaryings input) : SV_Target
{
    const float3 direction = SafeNormalize(input.DirectionWS);
    const float3 linearHdr = g_BackgroundEnvironmentMap.SampleLevel(
        g_LinearClampSampler, direction, 0.0f).rgb;
    return float4(linearHdr, 1.0f);
}
