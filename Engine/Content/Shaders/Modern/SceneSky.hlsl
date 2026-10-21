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
    float4x4 g_ShadowCameraView;
    float4x4 g_AdditionalLightViewProjections[3];
    float4 g_DirectionalShadowCascadeSplits;
    float4 g_DirectionalShadowCascadeSettings;
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
    const float3 sky = g_EnvironmentMap.SampleLevel(g_EnvironmentSampler, direction, 0.0f).rgb;
    // The horizon should converge to the same aerial color as distant geometry.
    const float horizon = pow(1.0f - saturate(abs(direction.y)), 4.0f);
    const float fog = (g_FogColorDensity.w > 0.0f) ? horizon : 0.0f;
    float3 skyRadiance = lerp(sky, g_FogColorDensity.rgb, fog);
    if (g_FogDirectionalColorIntensity.w > 0.0f &&
        g_DirectionalDirectionAndIntensity.w > 0.0f)
    {
        // Scene lights store the direction the rays travel, not the vector to the sun.
        const float3 sunDirection = -normalize(g_DirectionalDirectionAndIntensity.xyz);
        const float sunward = pow(saturate(dot(direction, sunDirection)),
            max(g_FogDirectionalParams.x, 1.0f));
        skyRadiance += g_FogDirectionalColorIntensity.rgb *
            (g_FogDirectionalColorIntensity.w * sunward);
    }
    return float4(skyRadiance * g_FogDirectionalParams.yzw *
        exp2(g_FogDistanceHeight.w), 1.0f);
}
