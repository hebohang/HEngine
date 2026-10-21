#pragma htype vs VS
#pragma htype ps PS

#include "Shared/BackendPushConstants.hlsli"

struct ToneMappingConstants
{
    float Exposure;
    uint PbrDebugMode;
    uint GiDebugView;
    float Upscale;
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

// Nine bilinear fetches reproduce the separable 4x4 Catmull-Rom kernel.
// Applied to the anti-aliased HDR scene only; UI is composed at native size later.
float3 SampleScene(float2 uv)
{
    if (g_ToneMappingConstants.Upscale < 0.5f || g_PbrDebugMode != 0u ||
        g_ToneMappingConstants.GiDebugView != 0u)
        return g_HdrSceneColor.SampleLevel(g_LinearClampSampler, uv, 0).rgb;
    uint width, height;
    g_HdrSceneColor.GetDimensions(width, height);
    const float2 size = float2(width, height);
    const float2 position = uv * size;
    const float2 center = floor(position - 0.5f) + 0.5f;
    const float2 f = position - center;
    const float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    const float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    const float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    const float2 w3 = f * f * (-0.5f + 0.5f * f);
    const float2 w12 = w1 + w2;
    const float2 coordinates[3] = {
        (center - 1.0f) / size, (center + w2 / w12) / size, (center + 2.0f) / size };
    const float2 weights[3] = {w0, w12, w3};
    float3 result = 0.0f, minimum = c_MaxFiniteHalf, maximum = 0.0f;
    [unroll] for (uint y = 0; y < 3; ++y)
    [unroll] for (uint x = 0; x < 3; ++x)
    {
        const float3 tap = SanitizeHdr(g_HdrSceneColor.SampleLevel(g_LinearClampSampler,
            float2(coordinates[x].x, coordinates[y].y), 0).rgb);
        result += tap * weights[x].x * weights[y].y;
        minimum = min(minimum, tap);
        maximum = max(maximum, tap);
    }
    return clamp(result, minimum, maximum);
}

float4 PS(FullscreenVaryings input) : SV_Target0
{
    const float3 rawHdr = SampleScene(input.TexCoord);
    // GI property views are data, not radiance. Map signed normals before
    // sanitization; PFM/raw capture remains upstream of all display mappings.
    const uint giView = g_ToneMappingConstants.GiDebugView;
    if (giView == 19u)
    {
        const float3 colors[9] = { float3(0,0,0), float3(1,1,0), float3(.05,.1,.35),
            float3(0,.7,1), float3(.4,.4,.4), float3(1,0,1), float3(1,.15,0),
            float3(.5,0,0), float3(1,1,1) };
        return float4(LinearToSRGB(colors[min(uint(max(rawHdr.x, 0.0f)), 8u)]), 1.0f);
    }
    if (giView == 4u || giView == 8u || (giView >= 10u && giView <= 14u) ||
        giView == 17u || giView == 18u || giView == 20u)
    {
        float3 data = all(isfinite(rawHdr)) ? rawHdr : 0.0f;
        if (giView == 20u) data.z = log2(1.0f + max(data.z, 0.0f)) / 16.0f;
        if (giView == 8u) data = data * 0.5f + 0.5f;
        else if (giView == 11u) data /= 32.0f;
        else if (giView == 10u || giView == 17u)
            data = log2(1.0f + max(data, 0.0f)) / log2(101.0f); // 0..100 world units.
        else if (giView == 14u)
            data = log2(1.0f + sqrt(max(data, 0.0f))) / 16.0f;
        return float4(LinearToSRGB(saturate(data)), 1.0f);
    }
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
        ? clamp(g_Exposure, 0.0f, 16777216.0f)
        : 1.0f;
    // Material/property data views are already normalized linear values. HDR
    // contribution views and FinalHdr retain the normal exposure/ACES path.
    const bool bNormalizedDebugView =
        (g_PbrDebugMode >= 1u && g_PbrDebugMode <= 5u) ||
        g_PbrDebugMode == 9u || g_PbrDebugMode == 12u || g_PbrDebugMode == 13u;
    const float3 displayLinear = bNormalizedDebugView
        ? saturate(linearHdr)
        : ToneMapACES(SanitizeHdr(linearHdr * safeExposure));

    // The current DX12 swapchain is RGBA8_UNORM, so apply the sRGB transfer
    // function explicitly. Switch this off when the swapchain view is sRGB.
    return float4(LinearToSRGB(displayLinear), 1.0f);
}
