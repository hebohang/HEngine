#pragma htype vs VS
#pragma htype ps PS

#include "Shared/BackendPushConstants.hlsli"

struct FxaaConstants
{
    uint2 Dimensions;
    float EdgeThreshold;
    float EdgeThresholdMin;
    float SubpixelQuality;
    float3 Padding;
};
HE_DECLARE_PUSH_CONSTANTS(FxaaConstants, g_FxaaConstants, space3);

Texture2D<float4> g_LdrSource : register(t0, space3);
SamplerState g_LinearClampSampler : register(s0, space3);

struct FullscreenVaryings
{
    float4 PositionCS : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

FullscreenVaryings VS(uint vertexId : SV_VertexID)
{
    const float2 position = float2(
        vertexId == 2u ? 3.0f : -1.0f,
        vertexId == 1u ? 3.0f : -1.0f);
    FullscreenVaryings output;
    output.PositionCS = float4(position, 0.0f, 1.0f);
    output.TexCoord = float2(
        position.x * 0.5f + 0.5f,
        0.5f - position.y * 0.5f);
    return output;
}

float Luminance(float3 color)
{
    return dot(color, float3(0.299f, 0.587f, 0.114f));
}

float SampleLuminance(float2 uv)
{
    return Luminance(g_LdrSource.SampleLevel(
        g_LinearClampSampler,
        uv,
        0.0f).rgb);
}

float SearchStep(uint iteration)
{
    // FXAA 3.11 PC quality preset 39. The first 1-pixel probe is issued
    // before the loop; these are P1..P11 of the canonical schedule.
    if (iteration < 4u)
        return 1.0f;
    if (iteration == 4u)
        return 1.5f;
    if (iteration < 9u)
        return 2.0f;
    if (iteration == 9u)
        return 4.0f;
    return 8.0f;
}

float4 PS(FullscreenVaryings input) : SV_Target0
{
    const uint2 dimensions = max(g_FxaaConstants.Dimensions, 1u);
    const float2 inverseDimensions = rcp(float2(dimensions));
    const float2 uv = input.TexCoord;
    const float4 center = g_LdrSource.SampleLevel(
        g_LinearClampSampler,
        uv,
        0.0f);
    if (!all(isfinite(center)))
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    const float lumaCenter = Luminance(center.rgb);
    const float lumaNorth = SampleLuminance(uv + float2(0.0f, -inverseDimensions.y));
    const float lumaSouth = SampleLuminance(uv + float2(0.0f, inverseDimensions.y));
    const float lumaWest = SampleLuminance(uv + float2(-inverseDimensions.x, 0.0f));
    const float lumaEast = SampleLuminance(uv + float2(inverseDimensions.x, 0.0f));
    const float lumaMinimum = min(
        lumaCenter,
        min(min(lumaNorth, lumaSouth), min(lumaWest, lumaEast)));
    const float lumaMaximum = max(
        lumaCenter,
        max(max(lumaNorth, lumaSouth), max(lumaWest, lumaEast)));
    const float lumaRange = lumaMaximum - lumaMinimum;
    const float edgeThreshold = max(
        g_FxaaConstants.EdgeThresholdMin,
        lumaMaximum * g_FxaaConstants.EdgeThreshold);
    if (!isfinite(lumaRange) || lumaRange < edgeThreshold)
        return center;

    const float lumaNorthWest = SampleLuminance(uv + float2(
        -inverseDimensions.x,
        -inverseDimensions.y));
    const float lumaNorthEast = SampleLuminance(uv + float2(
        inverseDimensions.x,
        -inverseDimensions.y));
    const float lumaSouthWest = SampleLuminance(uv + float2(
        -inverseDimensions.x,
        inverseDimensions.y));
    const float lumaSouthEast = SampleLuminance(uv + inverseDimensions);

    const float horizontalEdge =
        abs(-2.0f * lumaWest + lumaNorthWest + lumaSouthWest) +
        2.0f * abs(-2.0f * lumaCenter + lumaNorth + lumaSouth) +
        abs(-2.0f * lumaEast + lumaNorthEast + lumaSouthEast);
    const float verticalEdge =
        abs(-2.0f * lumaNorth + lumaNorthWest + lumaNorthEast) +
        2.0f * abs(-2.0f * lumaCenter + lumaWest + lumaEast) +
        abs(-2.0f * lumaSouth + lumaSouthWest + lumaSouthEast);
    const bool bHorizontal = horizontalEdge >= verticalEdge;

    const float lumaNegative = bHorizontal ? lumaNorth : lumaWest;
    const float lumaPositive = bHorizontal ? lumaSouth : lumaEast;
    const float gradientNegative = abs(lumaNegative - lumaCenter);
    const float gradientPositive = abs(lumaPositive - lumaCenter);
    const bool bUseNegative = gradientNegative >= gradientPositive;
    const float gradient = max(gradientNegative, gradientPositive);
    const float localAverage = 0.5f * (
        lumaCenter + (bUseNegative ? lumaNegative : lumaPositive));
    const float2 normalStep = bHorizontal
        ? float2(0.0f, inverseDimensions.y)
        : float2(inverseDimensions.x, 0.0f);
    const float2 edgeStep = bHorizontal
        ? float2(inverseDimensions.x, 0.0f)
        : float2(0.0f, inverseDimensions.y);
    const float2 edgeCenter = uv + normalStep * (bUseNegative ? -0.5f : 0.5f);

    float2 negativeUv = edgeCenter - edgeStep;
    float2 positiveUv = edgeCenter + edgeStep;
    float negativeDelta = SampleLuminance(negativeUv) - localAverage;
    float positiveDelta = SampleLuminance(positiveUv) - localAverage;
    const float searchThreshold = gradient * 0.25f;
    bool bNegativeDone = abs(negativeDelta) >= searchThreshold;
    bool bPositiveDone = abs(positiveDelta) >= searchThreshold;
    [unroll]
    for (uint iteration = 0u; iteration < 11u; ++iteration)
    {
        if (bNegativeDone && bPositiveDone)
            break;
        const float stepScale = SearchStep(iteration);
        if (!bNegativeDone)
        {
            negativeUv -= edgeStep * stepScale;
            negativeDelta = SampleLuminance(negativeUv) - localAverage;
            bNegativeDone = abs(negativeDelta) >= searchThreshold;
        }
        if (!bPositiveDone)
        {
            positiveUv += edgeStep * stepScale;
            positiveDelta = SampleLuminance(positiveUv) - localAverage;
            bPositiveDone = abs(positiveDelta) >= searchThreshold;
        }
    }

    const float distanceNegative = bHorizontal
        ? uv.x - negativeUv.x
        : uv.y - negativeUv.y;
    const float distancePositive = bHorizontal
        ? positiveUv.x - uv.x
        : positiveUv.y - uv.y;
    const bool bNegativeCloser = distanceNegative < distancePositive;
    const float nearestDistance = bNegativeCloser
        ? distanceNegative
        : distancePositive;
    const float totalDistance = max(
        distanceNegative + distancePositive,
        1e-6f);
    const float nearestDelta = bNegativeCloser
        ? negativeDelta
        : positiveDelta;
    const bool bCenterDarker = lumaCenter < localAverage;
    const bool bCorrectVariation = (nearestDelta < 0.0f) != bCenterDarker;
    const float edgeOffset = bCorrectVariation
        ? max(0.5f - nearestDistance / totalDistance, 0.0f)
        : 0.0f;

    const float neighborhoodAverage = (
        2.0f * (lumaNorth + lumaSouth + lumaWest + lumaEast) +
        lumaNorthWest + lumaNorthEast + lumaSouthWest + lumaSouthEast) /
        12.0f;
    float subpixelOffset = saturate(
        abs(neighborhoodAverage - lumaCenter) / max(lumaRange, 1e-6f));
    subpixelOffset = subpixelOffset * subpixelOffset *
        (3.0f - 2.0f * subpixelOffset);
    subpixelOffset = subpixelOffset * subpixelOffset *
        saturate(g_FxaaConstants.SubpixelQuality);

    // edgeOffset is already bounded to the canonical half-pixel range. Do not
    // attenuate it here: thresholds and SubpixelQuality are the intended
    // controls for detail preservation.
    const float finalOffset = max(edgeOffset, subpixelOffset);
    const float2 finalUv = uv + normalStep *
        (bUseNegative ? -finalOffset : finalOffset);
    float4 result = g_LdrSource.SampleLevel(
        g_LinearClampSampler,
        finalUv,
        0.0f);
    if (!all(isfinite(result)))
        result = center;
    result.a = center.a;
    return saturate(result);
}
