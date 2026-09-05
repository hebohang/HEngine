#pragma htype vs VS
#pragma htype ps PS
#pragma hvariant MULTISAMPLE

#include "Shared/BackendPushConstants.hlsli"

struct GlobalIlluminationCompositeConstants
{
    uint2 OutputDimensions;
    uint ScalarInput;
    uint SampleCount;
};
HE_DECLARE_PUSH_CONSTANTS(
    GlobalIlluminationCompositeConstants,
    g_CompositeConstants,
    space3);

Texture2D<float4> g_GlobalIllumination : register(t0, space3);
#if MULTISAMPLE
Texture2DMS<float> g_MultisampleDepth : register(t1, space3);
Texture2D<float> g_RepresentativeDepth : register(t2, space3);
#endif

struct FullscreenVaryings
{
    float4 Position : SV_Position;
};

FullscreenVaryings VS(uint vertexId : SV_VertexID)
{
    const float2 position = float2(
        vertexId == 2u ? 3.0f : -1.0f,
        vertexId == 1u ? 3.0f : -1.0f);
    FullscreenVaryings output;
    output.Position = float4(position, 0.0f, 1.0f);
    return output;
}

float3 LoadCompositeColor(FullscreenVaryings input)
{
    uint sourceWidth = 0u;
    uint sourceHeight = 0u;
    g_GlobalIllumination.GetDimensions(sourceWidth, sourceHeight);
    const uint2 outputDimensions = max(
        g_CompositeConstants.OutputDimensions,
        1u);
    const uint2 sourceDimensions = max(uint2(sourceWidth, sourceHeight), 1u);
    const uint2 sourcePixel = min(
        uint2(input.Position.xy *
            float2(sourceDimensions) / float2(outputDimensions)),
        sourceDimensions - 1u);
    const float4 sampleValue = g_GlobalIllumination.Load(
        int3(sourcePixel, 0));
    const float3 color = g_CompositeConstants.ScalarInput != 0u
        ? sampleValue.xxx
        : sampleValue.rgb;
    return color;
}

#if MULTISAMPLE
struct MultisampleCompositeOutput
{
    float4 Color : SV_Target0;
    uint Coverage : SV_Coverage;
};

MultisampleCompositeOutput PS(FullscreenVaryings input)
{
    MultisampleCompositeOutput output;
    output.Color = float4(LoadCompositeColor(input), 1.0f);
    output.Coverage = 0u;
    const uint2 pixel = min(
        uint2(input.Position.xy),
        max(g_CompositeConstants.OutputDimensions, 1u) - 1u);
    const uint sampleCount = clamp(
        g_CompositeConstants.SampleCount,
        1u,
        8u);
    const float representativeDepth = g_RepresentativeDepth.Load(
        int3(pixel, 0));
    const float depthTolerance = max(
        abs(representativeDepth) * 0.002f,
        5e-5f);
    [unroll]
    for (uint sampleIndex = 0u; sampleIndex < 8u; ++sampleIndex)
    {
        if (sampleIndex >= sampleCount)
            break;
        const float depth = g_MultisampleDepth.Load(pixel, sampleIndex);
        if (isfinite(depth) && isfinite(representativeDepth) &&
            depth < 1.0f && representativeDepth < 1.0f &&
            abs(depth - representativeDepth) <= depthTolerance)
            output.Coverage |= 1u << sampleIndex;
    }
    return output;
}
#else
float4 PS(FullscreenVaryings input) : SV_Target0
{
    return float4(LoadCompositeColor(input), 1.0f);
}
#endif
