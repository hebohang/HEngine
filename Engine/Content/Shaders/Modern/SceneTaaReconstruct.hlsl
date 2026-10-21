#pragma htype cs CS
#include "Shared/BackendPushConstants.hlsli"

struct ReconstructConstants
{
    uint2 Dimensions;
    float2 JitterPixels;
};
HE_DECLARE_PUSH_CONSTANTS(ReconstructConstants, g_Constants, space3);
Texture2D<float4> g_Source : register(t0, space3);
RWTexture2D<float4> g_Output : register(u0, space3);
SamplerState g_LinearClamp : register(s0, space3);

// History uses the jittered raster grid and its matching motion vectors.
// Reconstruct only the presentation copy on the unjittered grid, before
// transparent overlays. A positive projection jitter moves raster pixels
// right/down, so the stable output samples that same positive offset.
[numthreads(8, 8, 1)]
void CS(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (any(pixel >= g_Constants.Dimensions)) return;
    float2 size = float2(g_Constants.Dimensions);
    float2 position = float2(pixel) + 0.5f + g_Constants.JitterPixels;
    float2 center = floor(position - 0.5f) + 0.5f;
    float2 f = position - center;
    float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    float2 w3 = f * f * (-0.5f + 0.5f * f);
    float2 w12 = w1 + w2;
    float2 coordinates[3] = {(center - 1.0f) / size,
        (center + w2 / w12) / size, (center + 2.0f) / size};
    float2 weights[3] = {w0, w12, w3};
    float4 result = 0.0f, minimum = 65504.0f, maximum = 0.0f;
    [unroll] for (uint y = 0; y < 3; ++y)
    [unroll] for (uint x = 0; x < 3; ++x)
    {
        float4 tap = g_Source.SampleLevel(g_LinearClamp,
            float2(coordinates[x].x, coordinates[y].y), 0);
        tap = all(isfinite(tap)) ? clamp(tap, 0.0f, 65504.0f) : 0.0f;
        result += tap * weights[x].x * weights[y].y;
        minimum = min(minimum, tap);
        maximum = max(maximum, tap);
    }
    g_Output[pixel] = clamp(result, minimum, maximum);
}
