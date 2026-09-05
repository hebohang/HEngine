#include "Include/Common.hlsli"
#include "Include/PBR/Sampling.hlsli"

#pragma htype vs VS
#pragma htype ps PS

Texture2D g_EquirectangularMap : register(t4, space3);

cbuffer IBLCapture : register(b3, space3)
{
    float4x4 g_CaptureViewProjection;
    float g_Roughness;
    uint g_CaptureFaceIndex;
    float2 g_CaptureInvSize;
    float g_SourceCubemapSize;
    float3 _IBLCapturePadding;
}

struct CubeCaptureVaryings
{
    float4 PositionHS : SV_POSITION;
};

CubeCaptureVaryings VS(VertexIn input)
{
    CubeCaptureVaryings output;
    output.PositionHS = mul(g_CaptureViewProjection, float4(input.pos, 1.0f));
    return output;
}

float4 PS(CubeCaptureVaryings input) : SV_Target
{
    const float3 direction = CubeCaptureDirection(g_CaptureFaceIndex, input.PositionHS, g_CaptureInvSize);
    const float2 uv = DirectionToEquirectangularTextureUV(direction);
    const float3 color = g_EquirectangularMap.Sample(g_LinearClampSampler, uv).rgb;
    return float4(color, 1.0f);
}
