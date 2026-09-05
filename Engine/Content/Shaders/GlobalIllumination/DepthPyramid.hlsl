#pragma htype cs CS

#include "Shared/BackendPushConstants.hlsli"

struct DepthPyramidConstants
{
    uint2 SourceDimensions;
    uint2 OutputDimensions;
    uint CopySource;
    uint3 Padding;
};
HE_DECLARE_PUSH_CONSTANTS(
    DepthPyramidConstants,
    g_DepthPyramidConstants,
    space3);

Texture2D<float> g_SourceDepth : register(t0, space3);
RWTexture2D<float> g_OutputDepth : register(u0, space3);

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 outputPixel = dispatchThreadId.xy;
    if (any(outputPixel >= g_DepthPyramidConstants.OutputDimensions))
        return;

    if (g_DepthPyramidConstants.CopySource != 0u)
    {
        g_OutputDepth[outputPixel] =
            g_SourceDepth.Load(int3(outputPixel, 0));
        return;
    }

    const uint2 sourceBegin = outputPixel * 2u;
    uint2 sourceEnd = min(
        sourceBegin + 2u,
        g_DepthPyramidConstants.SourceDimensions);

    // Standard texture mip dimensions use floor division. Assign the odd
    // remainder to the last destination texel so every source depth sample
    // contributes and the hierarchy stays conservative.
    if (outputPixel.x + 1u == g_DepthPyramidConstants.OutputDimensions.x)
        sourceEnd.x = g_DepthPyramidConstants.SourceDimensions.x;
    if (outputPixel.y + 1u == g_DepthPyramidConstants.OutputDimensions.y)
        sourceEnd.y = g_DepthPyramidConstants.SourceDimensions.y;

    // HEngine's current raster depth is normal-Z device depth: background is
    // 1 and a smaller value is the nearer occluder.
    float minimumDepth = 1.0f;
    for (uint sourceY = sourceBegin.y; sourceY < sourceEnd.y; ++sourceY)
    {
        for (uint sourceX = sourceBegin.x; sourceX < sourceEnd.x; ++sourceX)
        {
            minimumDepth = min(
                minimumDepth,
                g_SourceDepth.Load(int3(uint2(sourceX, sourceY), 0)));
        }
    }
    g_OutputDepth[outputPixel] = minimumDepth;
}
