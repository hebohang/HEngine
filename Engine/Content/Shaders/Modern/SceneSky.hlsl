#pragma htype vs VS
#pragma htype ps PS

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
};

TextureCube<float4> g_EnvironmentMap : register(t0, space3);
SamplerState g_EnvironmentSampler : register(s0, space3);

struct SkyVaryings
{
    float4 PositionCS : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

SkyVaryings VS(uint vertexId : SV_VertexID)
{
    SkyVaryings output;
    const float2 position = float2(
        (vertexId == 2) ? 3.0f : -1.0f,
        (vertexId == 1) ? 3.0f : -1.0f);
    output.PositionCS = float4(position, 1.0f, 1.0f);
    output.TexCoord = float2(position.x * 0.5f + 0.5f, 0.5f - position.y * 0.5f);
    return output;
}

float4 PS(SkyVaryings input) : SV_Target0
{
    const float2 ndc = float2(
        input.TexCoord.x * 2.0f - 1.0f,
        1.0f - input.TexCoord.y * 2.0f);
    const float4 positionWS = mul(g_InverseViewProjection, float4(ndc, 1.0f, 1.0f));
    const float3 worldPosition = positionWS.xyz / max(abs(positionWS.w), 1e-5f);
    const float3 direction = normalize(worldPosition - g_CameraPositionAndExposure.xyz);
    return float4(g_EnvironmentMap.SampleLevel(g_EnvironmentSampler, direction, 0.0f).rgb, 1.0f);
}
