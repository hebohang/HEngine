#pragma htype cs CS

#include "Shared/BackendPushConstants.hlsli"

struct TaaConstants
{
    uint2 Dimensions;
    float HistoryWeight;
    float Sharpness;
    uint ResetHistory;
    float ViewDepthThreshold;
    float NormalRejectionCosine;
    float MotionRejectionPixels;
    uint HasTemporalGiConfidence;
    float LuminanceReactiveScale;
    float TemporalGiHistoryCeiling;
    float Padding;
};
HE_DECLARE_PUSH_CONSTANTS(TaaConstants, g_TaaConstants, space3);

Texture2D<float4> g_CurrentColor : register(t0, space3);
Texture2D<float2> g_MotionVectors : register(t1, space3);
Texture2D<float> g_CurrentDepth : register(t2, space3);
Texture2D<float2> g_CurrentViewDepth : register(t3, space3);
Texture2D<float4> g_CurrentNormalRoughness : register(t4, space3);
Texture2D<uint2> g_CurrentSurfaceId : register(t5, space3);
Texture2D<float> g_TemporalGiConfidence : register(t6, space3);
Texture2D<float4> g_PreviousHistoryColor : register(t7, space3);
Texture2D<float> g_PreviousHistoryViewDepth : register(t8, space3);
Texture2D<float4> g_PreviousHistoryNormalLength : register(t9, space3);
Texture2D<uint2> g_PreviousHistorySurfaceId : register(t10, space3);

RWTexture2D<float4> g_CurrentHistoryColor : register(u0, space3);
RWTexture2D<float> g_CurrentHistoryViewDepth : register(u1, space3);
RWTexture2D<float4> g_CurrentHistoryNormalLength : register(u2, space3);
RWTexture2D<uint2> g_CurrentHistorySurfaceId : register(u3, space3);
RWTexture2D<float4> g_ResolvedColor : register(u4, space3);

static const uint c_InvalidSurfaceId = 0xffffffffu;

float3 SanitizeColor(float3 color)
{
    return all(isfinite(color))
        ? clamp(color, 0.0f, 65504.0f)
        : 0.0f;
}

float3 RgbToYCoCg(float3 color)
{
    return float3(
        color.r * 0.25f + color.g * 0.5f + color.b * 0.25f,
        color.r * 0.5f - color.b * 0.5f,
        -color.r * 0.25f + color.g * 0.5f - color.b * 0.25f);
}

float3 YCoCgToRgb(float3 color)
{
    return float3(
        color.x + color.y - color.z,
        color.x + color.z,
        color.x - color.y - color.z);
}

float CatmullRomWeight(float value)
{
    const float distance = abs(value);
    if (distance <= 1.0f)
    {
        return 1.5f * distance * distance * distance -
            2.5f * distance * distance + 1.0f;
    }
    if (distance < 2.0f)
    {
        return -0.5f * distance * distance * distance +
            2.5f * distance * distance - 4.0f * distance + 2.0f;
    }
    return 0.0f;
}

bool IsValidSurfaceId(uint2 surfaceId)
{
    return all(surfaceId != c_InvalidSurfaceId);
}

bool IsHistoryGuideValid(
    int2 samplePixel,
    bool bSurface,
    float expectedPreviousViewDepth,
    float3 currentNormal,
    uint2 currentSurfaceId)
{
    const uint2 historySurfaceId =
        g_PreviousHistorySurfaceId.Load(int3(samplePixel, 0));
    const float historyViewDepth =
        g_PreviousHistoryViewDepth.Load(int3(samplePixel, 0));
    if (!bSurface)
    {
        return !IsValidSurfaceId(historySurfaceId) &&
            isfinite(historyViewDepth) && historyViewDepth <= 0.0f;
    }

    if (any(historySurfaceId != currentSurfaceId) ||
        !isfinite(historyViewDepth) || historyViewDepth <= 0.0f ||
        !isfinite(expectedPreviousViewDepth) ||
        expectedPreviousViewDepth <= 0.0f)
    {
        return false;
    }
    const float depthTolerance = max(
        0.0025f,
        expectedPreviousViewDepth *
            max(g_TaaConstants.ViewDepthThreshold, 1e-4f));
    if (abs(historyViewDepth - expectedPreviousViewDepth) > depthTolerance)
        return false;

    const float3 historyNormal =
        g_PreviousHistoryNormalLength.Load(int3(samplePixel, 0)).xyz;
    const float historyNormalLength = length(historyNormal);
    if (!all(isfinite(historyNormal)) || historyNormalLength <= 1e-5f)
        return false;
    return dot(currentNormal, historyNormal / historyNormalLength) >=
        saturate(g_TaaConstants.NormalRejectionCosine);
}

bool IsCurrentGuideValid(
    int2 samplePixel,
    bool bSurface,
    float3 currentNormal,
    uint2 currentSurfaceId)
{
    const float sampleDeviceDepth =
        g_CurrentDepth.Load(int3(samplePixel, 0));
    const uint2 sampleSurfaceId =
        g_CurrentSurfaceId.Load(int3(samplePixel, 0));
    if (!bSurface)
    {
        return isfinite(sampleDeviceDepth) && sampleDeviceDepth >= 1.0f &&
            !IsValidSurfaceId(sampleSurfaceId);
    }
    if (!isfinite(sampleDeviceDepth) || sampleDeviceDepth >= 1.0f ||
        any(sampleSurfaceId != currentSurfaceId))
    {
        return false;
    }
    const float3 sampleNormal =
        g_CurrentNormalRoughness.Load(int3(samplePixel, 0)).xyz;
    const float sampleNormalLength = length(sampleNormal);
    return all(isfinite(sampleNormal)) && sampleNormalLength > 1e-5f &&
        dot(currentNormal, sampleNormal / sampleNormalLength) >=
            saturate(g_TaaConstants.NormalRejectionCosine);
}

struct HistorySample
{
    float3 Color;
    float Length;
    bool bValid;
};

HistorySample SampleGuideValidatedHistory(
    float2 uv,
    uint2 dimensions,
    bool bSurface,
    float expectedPreviousViewDepth,
    float3 currentNormal,
    uint2 currentSurfaceId)
{
    const float2 samplePosition = uv * float2(dimensions) - 0.5f;
    const int2 basePixel = int2(floor(samplePosition));
    const float2 fraction = samplePosition - floor(samplePosition);
    float3 filteredColor = 0.0f;
    float filteredWeight = 0.0f;
    float3 closestColor = 0.0f;
    float closestLength = 0.0f;
    float closestDistance = 1e30f;
    bool bAllTapsValid = true;
    bool bFoundValidTap = false;
    [unroll]
    for (int y = -1; y <= 2; ++y)
    {
        const float weightY = CatmullRomWeight(float(y) - fraction.y);
        [unroll]
        for (int x = -1; x <= 2; ++x)
        {
            const float weight = weightY *
                CatmullRomWeight(float(x) - fraction.x);
            const int2 samplePixel = clamp(
                basePixel + int2(x, y),
                int2(0, 0),
                int2(dimensions) - 1);
            const bool bGuideValid = IsHistoryGuideValid(
                samplePixel,
                bSurface,
                expectedPreviousViewDepth,
                currentNormal,
                currentSurfaceId);
            bAllTapsValid = bAllTapsValid && bGuideValid;
            if (!bGuideValid)
                continue;

            const float3 sampleColor = SanitizeColor(
                g_PreviousHistoryColor.Load(int3(samplePixel, 0)).rgb);
            filteredColor += sampleColor * weight;
            filteredWeight += weight;
            const float2 sampleCenter = float2(samplePixel) + 0.5f;
            const float distanceSquared = dot(
                sampleCenter - uv * float2(dimensions),
                sampleCenter - uv * float2(dimensions));
            if (!bFoundValidTap || distanceSquared < closestDistance)
            {
                closestDistance = distanceSquared;
                closestColor = sampleColor;
                closestLength = max(
                    g_PreviousHistoryNormalLength.Load(
                        int3(samplePixel, 0)).w,
                    0.0f);
                bFoundValidTap = true;
            }
        }
    }

    HistorySample result;
    result.bValid = bFoundValidTap;
    result.Length = closestLength;
    result.Color = bAllTapsValid && abs(filteredWeight) > 1e-5f
        ? SanitizeColor(filteredColor / filteredWeight)
        : closestColor;
    return result;
}

void StoreResolved(
    uint2 pixel,
    float4 color,
    float3 currentNormal,
    float historyLength)
{
    g_CurrentHistoryColor[pixel] = color;
    g_CurrentHistoryNormalLength[pixel] = float4(
        currentNormal,
        clamp(historyLength, 1.0f, 64.0f));
    g_ResolvedColor[pixel] = color;
}

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 dimensions = max(g_TaaConstants.Dimensions, 1u);
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= dimensions))
        return;

    const float4 rawCurrent = g_CurrentColor.Load(int3(pixel, 0));
    const float4 current = float4(
        SanitizeColor(rawCurrent.rgb),
        isfinite(rawCurrent.a) ? saturate(rawCurrent.a) : 1.0f);
    const float currentDepthValue = g_CurrentDepth.Load(int3(pixel, 0));
    const float currentDepth = isfinite(currentDepthValue)
        ? saturate(currentDepthValue)
        : 1.0f;
    const float2 currentViewDepth =
        g_CurrentViewDepth.Load(int3(pixel, 0));
    const uint2 currentSurfaceId =
        g_CurrentSurfaceId.Load(int3(pixel, 0));
    const float3 rawCurrentNormal =
        g_CurrentNormalRoughness.Load(int3(pixel, 0)).xyz;
    const float currentNormalLength = length(rawCurrentNormal);
    const bool bSurface = currentDepth < 1.0f &&
        IsValidSurfaceId(currentSurfaceId) &&
        all(isfinite(currentViewDepth)) && currentViewDepth.x > 0.0f &&
        all(isfinite(rawCurrentNormal)) && currentNormalLength > 1e-5f;
    const float3 currentNormal = bSurface
        ? rawCurrentNormal / currentNormalLength
        : 0.0f;
    g_CurrentHistoryViewDepth[pixel] = bSurface
        ? currentViewDepth.x
        : 0.0f;
    g_CurrentHistorySurfaceId[pixel] = bSurface
        ? currentSurfaceId
        : uint2(c_InvalidSurfaceId, c_InvalidSurfaceId);

    // Surface guides currently cover static opaque geometry. A finite depth
    // with the invalid key belongs to an unsupported skinned/masked surface;
    // keep it current-only instead of reusing zero-velocity background history.
    if (!bSurface && currentDepth < 1.0f)
    {
        StoreResolved(pixel, current, 0.0f, 1.0f);
        return;
    }

    if (g_TaaConstants.ResetHistory != 0u)
    {
        StoreResolved(pixel, current, currentNormal, 1.0f);
        return;
    }

    const float2 motion = g_MotionVectors.Load(int3(pixel, 0));
    const float2 currentUv = (float2(pixel) + 0.5f) / float2(dimensions);
    const float2 previousUv = currentUv + motion;
    const float motionPixels = length(motion * float2(dimensions));
    const bool bMotionValid = all(isfinite(motion)) &&
        all(previousUv >= 0.0f) && all(previousUv <= 1.0f) &&
        isfinite(motionPixels) &&
        motionPixels <= max(g_TaaConstants.MotionRejectionPixels, 1.0f);
    if (!bMotionValid ||
        (bSurface && (!isfinite(currentViewDepth.y) ||
            currentViewDepth.y <= 0.0f)))
    {
        StoreResolved(pixel, current, currentNormal, 1.0f);
        return;
    }

    const HistorySample historySample = SampleGuideValidatedHistory(
        previousUv,
        dimensions,
        bSurface,
        currentViewDepth.y,
        currentNormal,
        currentSurfaceId);
    if (!historySample.bValid || !all(isfinite(historySample.Color)))
    {
        StoreResolved(pixel, current, currentNormal, 1.0f);
        return;
    }

    float3 neighborhoodMinimum = RgbToYCoCg(current.rgb);
    float3 neighborhoodMaximum = neighborhoodMinimum;
    float3 neighborhoodSum = 0.0f;
    float3 neighborhoodSquareSum = 0.0f;
    float3 neighborhoodRgbSum = 0.0f;
    float neighborhoodCount = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const int2 samplePixel = clamp(
                int2(pixel) + int2(x, y),
                int2(0, 0),
                int2(dimensions) - 1);
            if (!IsCurrentGuideValid(
                    samplePixel,
                    bSurface,
                    currentNormal,
                    currentSurfaceId))
            {
                continue;
            }
            const float3 sampleRgb = SanitizeColor(
                g_CurrentColor.Load(int3(samplePixel, 0)).rgb);
            const float3 sampleColor = RgbToYCoCg(sampleRgb);
            neighborhoodMinimum = min(neighborhoodMinimum, sampleColor);
            neighborhoodMaximum = max(neighborhoodMaximum, sampleColor);
            neighborhoodSum += sampleColor;
            neighborhoodSquareSum += sampleColor * sampleColor;
            neighborhoodRgbSum += sampleRgb;
            neighborhoodCount += 1.0f;
        }
    }

    neighborhoodCount = max(neighborhoodCount, 1.0f);
    const float3 neighborhoodAverage =
        neighborhoodRgbSum / neighborhoodCount;
    const float sharpness = saturate(g_TaaConstants.Sharpness) * 0.25f;
    const float3 sharpenedCurrent = SanitizeColor(
        current.rgb + (current.rgb - neighborhoodAverage) * sharpness);
    const float3 momentMean = neighborhoodSum / neighborhoodCount;
    const float3 momentVariance = max(
        neighborhoodSquareSum / neighborhoodCount - momentMean * momentMean,
        0.0f);
    const float3 momentSigma = sqrt(momentVariance);
    const float3 clipMinimum = max(
        neighborhoodMinimum,
        momentMean - momentSigma * 1.25f);
    const float3 clipMaximum = min(
        neighborhoodMaximum,
        momentMean + momentSigma * 1.25f);
    float3 history = clamp(
        RgbToYCoCg(historySample.Color),
        clipMinimum,
        clipMaximum);
    const bool bTemporalGi =
        g_TaaConstants.HasTemporalGiConfidence != 0u;
    // SVGF can still deliver a rare high-energy 1-spp observation at a pixel
    // whose history was geometrically rejected. Variance-clip that current
    // sample in the same-surface neighborhood before temporal blending; the
    // ordinary raster path keeps its full min/max range for crisp highlights.
    const float3 giCurrentMinimum = max(
        neighborhoodMinimum,
        momentMean - momentSigma * 0.75f);
    const float3 giCurrentMaximum = min(
        neighborhoodMaximum,
        momentMean + momentSigma * 0.75f);
    const float3 currentColor = clamp(
        RgbToYCoCg(sharpenedCurrent),
        bTemporalGi ? giCurrentMinimum : neighborhoodMinimum,
        bTemporalGi ? giCurrentMaximum : neighborhoodMaximum);

    const float motionConfidence = 1.0f - saturate(
        motionPixels / max(g_TaaConstants.MotionRejectionPixels, 1.0f));
    const float currentLuminance = max(currentColor.x, 0.0f);
    const float historyLuminance = max(
        RgbToYCoCg(historySample.Color).x,
        0.0f);
    const float relativeLuminanceChange =
        abs(currentLuminance - historyLuminance) /
        max(max(currentLuminance, historyLuminance), 0.05f);
    const float luminanceReactive = saturate(
        (relativeLuminanceChange - 0.05f) *
        max(g_TaaConstants.LuminanceReactiveScale, 0.0f));
    float reactive = luminanceReactive;
    if (g_TaaConstants.HasTemporalGiConfidence != 0u)
    {
        const float rawGiConfidence =
            g_TemporalGiConfidence.Load(int3(pixel, 0));
        const float giConfidence = isfinite(rawGiConfidence)
            ? saturate(rawGiConfidence)
            : 0.0f;
        // SVGF confidence already identifies lighting invalidation. Treating
        // a stable high-confidence stochastic radiance outlier as a generic
        // luminance-reactive surface would select the firefly as current-only
        // and make it survive TAA. Geometry/depth/normal/surface-ID guides
        // still reject disocclusion independently.
        reactive = (1.0f - giConfidence) * 0.75f;
    }

    const float previousHistoryLength = isfinite(historySample.Length)
        ? clamp(historySample.Length, 1.0f, 64.0f)
        : 1.0f;
    const float sampleCountCeiling = previousHistoryLength /
        (previousHistoryLength + 1.0f);
    const float configuredHistoryCeiling =
        g_TaaConstants.HasTemporalGiConfidence != 0u
        ? min(
            saturate(g_TaaConstants.HistoryWeight),
            saturate(g_TaaConstants.TemporalGiHistoryCeiling))
        : saturate(g_TaaConstants.HistoryWeight);
    const float historyWeight = min(
            configuredHistoryCeiling,
            sampleCountCeiling) *
        lerp(0.25f, 1.0f, motionConfidence) *
        (1.0f - reactive) * (1.0f - reactive);
    const float3 resolvedYCoCg = clamp(
        lerp(currentColor, history, historyWeight),
        neighborhoodMinimum,
        neighborhoodMaximum);
    const float3 resolved = SanitizeColor(YCoCgToRgb(resolvedYCoCg));
    const float nextHistoryLength = reactive >= 0.8f
        ? 1.0f
        : min(previousHistoryLength + 1.0f, 64.0f);
    StoreResolved(
        pixel,
        float4(resolved, current.a),
        currentNormal,
        nextHistoryLength);
}
