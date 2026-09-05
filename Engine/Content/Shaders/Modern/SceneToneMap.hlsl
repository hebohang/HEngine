#pragma htype vs VS
#pragma htype ps PS

#include "Shared/BackendPushConstants.hlsli"

struct ToneMappingConstants
{
    float Exposure;
    uint PbrDebugMode;
    float2 Padding;
};
HE_DECLARE_PUSH_CONSTANTS(ToneMappingConstants, g_ToneMappingConstants, space3);

#define g_Exposure g_ToneMappingConstants.Exposure
#define g_PbrDebugMode g_ToneMappingConstants.PbrDebugMode

Texture2D<float4> g_HdrSceneColor : register(t0, space3);
SamplerState g_LinearClampSampler : register(s0, space3);

static const float c_MaxFiniteHalf = 65504.0f;
static const uint c_PbrDebugHdrValidity = 11u;

struct FullscreenVaryings
{
    float4 PositionCS : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

FullscreenVaryings VS(uint vertexId : SV_VertexID)
{
    FullscreenVaryings output;
    const float2 position = float2(
        (vertexId == 2) ? 3.0f : -1.0f,
        (vertexId == 1) ? 3.0f : -1.0f);
    output.PositionCS = float4(position, 0.0f, 1.0f);
    output.TexCoord = float2(position.x * 0.5f + 0.5f, 0.5f - position.y * 0.5f);
    return output;
}

float3 ToneMapACES(float3 color)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float3 SanitizeHdr(float3 color)
{
    if (!all(isfinite(color)))
        return 0.0f;
    return clamp(color, 0.0f, c_MaxFiniteHalf);
}

float3 LinearToSRGB(float3 color)
{
    const float3 lower = color * 12.92f;
    const float3 upper = 1.055f * pow(max(color, 0.0f), 1.0f / 2.4f) - 0.055f;
    return lerp(upper, lower, color <= 0.0031308f);
}

float4 PS(FullscreenVaryings input) : SV_Target0
{
    const float3 rawHdr =
        g_HdrSceneColor.Sample(g_LinearClampSampler, input.TexCoord).rgb;
    if (g_PbrDebugMode == c_PbrDebugHdrValidity)
    {
        float3 validityColor = 0.0f;
        if (!all(isfinite(rawHdr)))
            validityColor = float3(1.0f, 0.0f, 1.0f); // Magenta: NaN/Inf.
        else if (any(rawHdr < 0.0f))
            validityColor = float3(0.0f, 1.0f, 1.0f); // Cyan: negative radiance.
        else if (any(rawHdr > 60000.0f))
            validityColor = float3(1.0f, 1.0f, 0.0f); // Yellow: near half overflow.
        return float4(LinearToSRGB(validityColor), 1.0f);
    }

    const float3 linearHdr = SanitizeHdr(rawHdr);
    const float safeExposure = isfinite(g_Exposure)
        ? clamp(g_Exposure, 0.0f, c_MaxFiniteHalf)
        : 1.0f;
    // Material/property data views are already normalized linear values. HDR
    // contribution views and FinalHdr retain the normal exposure/ACES path.
    const bool bNormalizedDebugView =
        (g_PbrDebugMode >= 1u && g_PbrDebugMode <= 5u) ||
        g_PbrDebugMode == 9u;
    const float3 displayLinear = bNormalizedDebugView
        ? saturate(linearHdr)
        : ToneMapACES(SanitizeHdr(linearHdr * safeExposure));

    // The current DX12 swapchain is RGBA8_UNORM, so apply the sRGB transfer
    // function explicitly. Switch this off when the swapchain view is sRGB.
    return float4(LinearToSRGB(displayLinear), 1.0f);
}
