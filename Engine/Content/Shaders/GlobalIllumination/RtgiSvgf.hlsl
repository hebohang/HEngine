#pragma htype cs CS
#pragma hvariant DEMODULATE
#pragma hvariant TEMPORAL
#pragma hvariant HISTORY_FIX
#pragma hvariant VARIANCE
#pragma hvariant UNCERTAINTY
#pragma hvariant ATROUS
#pragma hvariant ATROUS_CACHED
#pragma hvariant UPSAMPLE
#pragma hvariant REMODULATE

#include "Shared/BackendPushConstants.hlsli"

static const uint c_InvalidMaterial = 0xffffffffu;

float Luminance(float3 value)
{
    return dot(value, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 SafeNormal(float3 value)
{
    const float lengthSquared = dot(value, value);
    return all(isfinite(value)) && isfinite(lengthSquared) &&
        lengthSquared > 1e-20f
        ? value * rsqrt(lengthSquared)
        : 0.0f;
}

uint2 MapPixel(uint2 pixel, uint2 sourceDimensions, uint2 targetDimensions)
{
    return min(
        uint2((float2(pixel) + 0.5f) *
            float2(targetDimensions) / float2(sourceDimensions)),
        targetDimensions - 1u);
}

float3 DiffuseAlbedo(float4 baseColorMetalness, float shadingModel)
{
    return max(baseColorMetalness.rgb, 0.0f) *
        (1.0f - saturate(baseColorMetalness.w)) * (shadingModel < 0.5f ? 1.0f : 0.0f);
}

#if DEMODULATE
struct DemodulateConstants
{
    uint2 g_DemodulateTraceDimensions;
    uint2 g_DemodulateFullDimensions;
    float g_AlbedoEpsilon;
    uint g_DemodulateRasterSurface;
    float2 g_DemodulatePadding;
};
HE_DECLARE_PUSH_CONSTANTS(
    DemodulateConstants,
    g_DemodulateConstants,
    space3);
Texture2D<float4> g_RawDiffuse : register(t0, space3);
Texture2D<float4> g_DemodulateBaseColorMetalness : register(t1, space3);
Texture2D<float4> g_DemodulateEmissive : register(t2, space3);
RWTexture2D<float4> g_DemodulatedIllumination : register(u0, space3);

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= g_DemodulateConstants.g_DemodulateTraceDimensions))
        return;
    const uint2 fullPixel = MapPixel(
        pixel,
        g_DemodulateConstants.g_DemodulateTraceDimensions,
        g_DemodulateConstants.g_DemodulateFullDimensions);
    const float3 raw = max(g_RawDiffuse.Load(int3(pixel, 0)).rgb, 0.0f);
    const float4 baseColorMetalness =
        g_DemodulateBaseColorMetalness.Load(int3(fullPixel, 0));
    const float4 emissionModel = g_DemodulateEmissive.Load(int3(fullPixel, 0));
    const float3 emissive = max(emissionModel.rgb, 0.0f);
    const float3 albedo = DiffuseAlbedo(baseColorMetalness, emissionModel.a);
    const float epsilon = max(
        g_DemodulateConstants.g_AlbedoEpsilon,
        1e-6f);
    const bool bDiffuse = baseColorMetalness.w < 1.0f &&
        any(albedo > 0.0f);
    const bool bEmissive = g_DemodulateConstants.g_DemodulateRasterSurface == 0u && any(emissive > epsilon);
    const float3 illumination = bDiffuse && !bEmissive
        ? float3(
            albedo.r > 0.0f ? raw.r / max(albedo.r, epsilon) : 0.0f,
            albedo.g > 0.0f ? raw.g / max(albedo.g, epsilon) : 0.0f,
            albedo.b > 0.0f ? raw.b / max(albedo.b, epsilon) : 0.0f)
        : 0.0f;
    g_DemodulatedIllumination[pixel] = float4(
        all(isfinite(illumination)) ? illumination : 0.0f,
        bDiffuse && !bEmissive ? 1.0f : 0.0f);
}
#endif

#if TEMPORAL
struct TemporalConstants
{
    uint2 g_TraceDimensions;
    uint2 g_FullDimensions;
    uint g_ResetHistory;
    float g_DepthThreshold;
    float g_NormalThreshold;
    float g_MinimumBlend;
    float g_NeighborhoodClampSigma;
    float g_ViewDepthRelativeThreshold;
    uint g_TemporalOptimizedKernels;
    uint g_ScreenSpaceSignal;
    float g_MaximumHistoryLength;
    uint g_CountDiagnostics;
    uint2 g_TemporalPadding;
    uint4 g_ReactivePixels;
    uint g_LocalReactiveBudget;
    float g_StaticMinimumBlend;
    uint2 g_ReactivePadding;
};
HE_DECLARE_PUSH_CONSTANTS(TemporalConstants, g_TemporalConstants, space3);
#define g_TraceDimensions g_TemporalConstants.g_TraceDimensions
#define g_FullDimensions g_TemporalConstants.g_FullDimensions
#define g_ResetHistory g_TemporalConstants.g_ResetHistory
#define g_DepthThreshold g_TemporalConstants.g_DepthThreshold
#define g_NormalThreshold g_TemporalConstants.g_NormalThreshold
#define g_MinimumBlend g_TemporalConstants.g_MinimumBlend
#define g_NeighborhoodClampSigma \
    g_TemporalConstants.g_NeighborhoodClampSigma
#define g_ViewDepthRelativeThreshold \
    g_TemporalConstants.g_ViewDepthRelativeThreshold

Texture2D<float4> g_NoisyDiffuse : register(t0, space3);
Texture2D<float> g_HitDistance : register(t1, space3);
Texture2D<float> g_CurrentDepth : register(t2, space3);
Texture2D<float4> g_CurrentNormal : register(t3, space3);
Texture2D<float2> g_CurrentMotion : register(t4, space3);
Texture2D<uint> g_CurrentMaterial : register(t5, space3);
Texture2D<float> g_TraceConfidence : register(t6, space3);
Texture2D<float4> g_PreviousDiffuse : register(t7, space3);
Texture2D<float2> g_PreviousMoments : register(t8, space3);
Texture2D<float> g_PreviousHistoryLength : register(t9, space3);
Texture2D<float> g_PreviousDepth : register(t10, space3);
Texture2D<float4> g_PreviousNormal : register(t11, space3);
Texture2D<uint> g_PreviousMaterial : register(t12, space3);
Texture2D<float> g_ExpectedPreviousDepth : register(t13, space3);
Texture2D<float> g_PreviousHitDistance : register(t14, space3);
Texture2D<float> g_CurrentCoverage : register(t15, space3);
Texture2D<float2> g_CurrentViewDepth : register(t16, space3);
Texture2D<uint2> g_CurrentSurfaceId : register(t17, space3);
Texture2D<float> g_PreviousHistoryViewDepth : register(t18, space3);
Texture2D<uint2> g_PreviousHistorySurfaceId : register(t19, space3);
RWTexture2D<float4> g_HistoryDiffuse : register(u0, space3);
RWTexture2D<float2> g_HistoryMoments : register(u1, space3);
RWTexture2D<float> g_HistoryLength : register(u2, space3);
RWTexture2D<float> g_HistoryDepth : register(u3, space3);
RWTexture2D<float4> g_HistoryNormal : register(u4, space3);
RWTexture2D<uint> g_HistoryMaterial : register(u5, space3);
RWTexture2D<float> g_TemporalConfidence : register(u6, space3);
RWTexture2D<float> g_HistoryHitDistance : register(u7, space3);
RWTexture2D<float> g_HistoryViewDepth : register(u8, space3);
RWTexture2D<uint2> g_HistorySurfaceId : register(u9, space3);
RWStructuredBuffer<uint> g_DiagnosticCounters : register(u10, space3);

struct ReprojectedHistory
{
    float4 Diffuse;
    float2 Moments;
    float Length;
    float HitDistance;
    float ValidWeight;
    float GeometricWeight;
};

bool IsHistoryTapValid(
    int2 tapPixel,
    float expectedPreviousViewDepth,
    float expectedPreviousDepth,
    float3 currentNormal,
    uint currentMaterial,
    uint2 currentSurfaceId,
    out bool geometricValid)
{
    geometricValid = false;
    if (any(tapPixel < 0) || any(tapPixel >= int2(g_TraceDimensions)))
        return false;

    const float previousDepth = g_PreviousDepth.Load(int3(tapPixel, 0));
    const float3 previousNormal = SafeNormal(
        g_PreviousNormal.Load(int3(tapPixel, 0)).xyz);
    const uint previousMaterial = g_PreviousMaterial.Load(int3(tapPixel, 0));
    const float previousViewDepth = g_PreviousHistoryViewDepth.Load(
        int3(tapPixel, 0));
    const uint2 previousSurfaceId = g_PreviousHistorySurfaceId.Load(
        int3(tapPixel, 0));
    const float viewDepthTolerance = max(
        0.0025f,
        expectedPreviousViewDepth *
            max(g_ViewDepthRelativeThreshold, 1e-4f));
    geometricValid = previousMaterial == currentMaterial &&
        previousSurfaceId.x == currentSurfaceId.x &&
        isfinite(previousDepth) &&
        isfinite(expectedPreviousDepth) &&
        isfinite(previousViewDepth) && previousViewDepth > 0.0f &&
        isfinite(expectedPreviousViewDepth) &&
        expectedPreviousViewDepth > 0.0f &&
        abs(previousViewDepth - expectedPreviousViewDepth) <=
            viewDepthTolerance &&
        abs(previousDepth - expectedPreviousDepth) <= g_DepthThreshold &&
        dot(previousNormal, currentNormal) >= g_NormalThreshold;
    return geometricValid && all(previousSurfaceId == currentSurfaceId);
}

ReprojectedHistory GatherReprojectedHistory(
    float2 previousUv,
    float expectedPreviousViewDepth,
    float expectedPreviousDepth,
    float3 currentNormal,
    uint currentMaterial,
    uint2 currentSurfaceId)
{
    const float2 samplePosition =
        previousUv * float2(g_TraceDimensions) - 0.5f;
    const int2 basePixel = int2(floor(samplePosition));
    const float2 fraction = saturate(samplePosition - floor(samplePosition));
    ReprojectedHistory result = (ReprojectedHistory)0;
    [unroll]
    for (int y = 0; y <= 1; ++y)
    {
        [unroll]
        for (int x = 0; x <= 1; ++x)
        {
            const int2 tapPixel = basePixel + int2(x, y);
            const float2 axisWeight = float2(
                x == 0 ? 1.0f - fraction.x : fraction.x,
                y == 0 ? 1.0f - fraction.y : fraction.y);
            const float weight = axisWeight.x * axisWeight.y;
            if (weight <= 0.0f)
                continue;
            bool geometricValid;
            const bool historyValid = IsHistoryTapValid(
                    tapPixel,
                    expectedPreviousViewDepth,
                    expectedPreviousDepth,
                    currentNormal,
                    currentMaterial,
                    currentSurfaceId,
                    geometricValid);
            result.GeometricWeight += geometricValid ? weight : 0.0f;
            if (!historyValid)
            {
                continue;
            }

            const float4 diffuse = g_PreviousDiffuse.Load(int3(tapPixel, 0));
            const float2 moments = g_PreviousMoments.Load(int3(tapPixel, 0));
            const float lengthValue = g_PreviousHistoryLength.Load(
                int3(tapPixel, 0));
            const float hitDistance = g_PreviousHitDistance.Load(
                int3(tapPixel, 0));
            if (!all(isfinite(diffuse)) || !all(isfinite(moments)) ||
                !isfinite(lengthValue) || !isfinite(hitDistance))
            {
                continue;
            }

            result.Diffuse += diffuse * weight;
            result.Moments += moments * weight;
            result.Length += max(lengthValue, 0.0f) * weight;
            result.HitDistance += max(hitDistance, 0.0f) * weight;
            result.ValidWeight += weight;
        }
    }

    if (result.ValidWeight > 1e-5f)
    {
        const float inverseWeight = rcp(result.ValidWeight);
        result.Diffuse *= inverseWeight;
        result.Moments *= inverseWeight;
        result.Length *= inverseWeight;
        result.HitDistance *= inverseWeight;
    }
    return result;
}

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    const bool localBudget = g_TemporalConstants.g_LocalReactiveBudget != 0u;
    const bool reactivePixel = all(pixel >= g_TemporalConstants.g_ReactivePixels.xy) &&
        all(pixel < g_TemporalConstants.g_ReactivePixels.zw);
    const float maximumHistoryLength = localBudget && !reactivePixel
        ? 128.0f : g_TemporalConstants.g_MaximumHistoryLength;
    const float minimumBlend = localBudget && !reactivePixel
        ? g_TemporalConstants.g_StaticMinimumBlend : g_MinimumBlend;
    if (any(pixel >= g_TraceDimensions))
        return;
    const uint2 fullPixel = MapPixel(
        pixel,
        g_TraceDimensions,
        g_FullDimensions);
    const float4 noisy = g_NoisyDiffuse.Load(int3(pixel, 0));
    const float currentDepth = g_CurrentDepth.Load(int3(fullPixel, 0));
    const float3 currentNormal = SafeNormal(
        g_CurrentNormal.Load(int3(fullPixel, 0)).xyz);
    const uint currentMaterial = g_CurrentMaterial.Load(int3(fullPixel, 0));
    const float2 currentViewDepth = g_CurrentViewDepth.Load(
        int3(fullPixel, 0));
    const uint2 currentSurfaceId = g_CurrentSurfaceId.Load(
        int3(fullPixel, 0));
    const float traceConfidence = g_TraceConfidence.Load(int3(pixel, 0));
    const float currentCoverage = g_CurrentCoverage.Load(int3(pixel, 0));
    const float2 motion = g_CurrentMotion.Load(int3(fullPixel, 0));
    const float2 currentUv = (float2(pixel) + 0.5f) / float2(g_TraceDimensions);
    const float2 previousUv = currentUv + motion;
    const bool bInside = all(previousUv >= 0.0f) && all(previousUv < 1.0f);

    const float observationWeight = isfinite(traceConfidence)
        ? saturate(traceConfidence)
        : 0.0f;
    const float observationCoverage = isfinite(currentCoverage)
        ? saturate(currentCoverage)
        : 0.0f;
    const bool bHasObservation = observationWeight > 0.0f &&
        observationCoverage > 0.0f;
    // Surface reprojection validity and stochastic-sampling availability are
    // different contracts. A stable surface with no screen-space hit must
    // retain its previous estimate instead of resetting or blending in black.
    const bool bCurrentSurfaceValid =
        currentMaterial != c_InvalidMaterial && currentDepth < 1.0f &&
        all(currentSurfaceId != c_InvalidMaterial) &&
        all(isfinite(currentViewDepth)) && currentViewDepth.x > 0.0f;
    bool bValid = g_ResetHistory == 0u && bInside &&
        bCurrentSurfaceValid && currentViewDepth.y > 0.0f;
    g_HistoryViewDepth[pixel] = bCurrentSurfaceValid
        ? currentViewDepth.x
        : 0.0f;
    g_HistorySurfaceId[pixel] = bCurrentSurfaceValid
        ? currentSurfaceId
        : uint2(c_InvalidMaterial, c_InvalidMaterial);
    float4 previousDiffuse = 0.0f;
    float2 previousMoments = 0.0f;
    float previousLength = 0.0f;
    float previousHitDistance = 0.0f;
    if (bValid)
    {
        const float expectedPreviousDepth = g_ExpectedPreviousDepth.Load(
            int3(fullPixel, 0));
        const ReprojectedHistory reprojected = GatherReprojectedHistory(
            previousUv,
            currentViewDepth.y,
            expectedPreviousDepth,
            currentNormal,
            currentMaterial,
            currentSurfaceId);
        // A primitive boundary inside a continuous surface can leave a
        // minority of matching-ID taps even though the entire footprint has
        // valid geometry. SSGI may retain those same-primitive colors only
        // when every weighted tap passes the existing object/depth/normal
        // gates. A silhouette or disocclusion still requires majority support;
        // colors from another primitive are never admitted by this fallback.
        const bool bSupportedInternalEdge =
            g_TemporalConstants.g_ScreenSpaceSignal != 0u &&
            reprojected.GeometricWeight >= 1.0f - 1e-5f &&
            reprojected.ValidWeight > 1e-5f;
        bValid = reprojected.ValidWeight >= 0.5f || bSupportedInternalEdge;
        previousDiffuse = reprojected.Diffuse;
        previousMoments = reprojected.Moments;
        previousLength = reprojected.Length;
        previousLength = isfinite(previousLength)
            ? clamp(previousLength, 0.0f, maximumHistoryLength)
            : 0.0f;
        previousHitDistance = reprojected.HitDistance;
    }

    if (g_TemporalConstants.g_CountDiagnostics != 0u)
    {
        uint reason = !bCurrentSurfaceValid ? 0u : g_ResetHistory != 0u ? 4u :
            !bInside ? 5u : !(currentViewDepth.y > 0.0f) ? 6u : !bValid ? 7u : 0u;
        const uint counts[8] = { WaveActiveSum(1u), WaveActiveSum(bCurrentSurfaceValid ? 1u : 0u),
            WaveActiveSum(bValid ? 1u : 0u), WaveActiveSum(bCurrentSurfaceValid && !bValid ? 1u : 0u),
            WaveActiveSum(reason == 4u ? 1u : 0u), WaveActiveSum(reason == 5u ? 1u : 0u),
            WaveActiveSum(reason == 6u ? 1u : 0u), WaveActiveSum(reason == 7u ? 1u : 0u) };
        if (WaveIsFirstLane())
            for (uint counter = 0u; counter < 8u; ++counter)
                InterlockedAdd(g_DiagnosticCounters[counter], counts[counter]);
    }

    // These statistics only affect valid observed history. Missing SSGI
    // observations must still retain and age history through the common tail.
    [branch]
    if (g_TemporalConstants.g_TemporalOptimizedKernels == 0u || (bValid && bHasObservation))
    {
    float3 neighborhoodMinimum = noisy.rgb;
    float3 neighborhoodMaximum = noisy.rgb;
    float3 neighborhoodSum = noisy.rgb;
    float3 neighborhoodSquaredSum = noisy.rgb * noisy.rgb;
    const float noisyLuminance = Luminance(noisy.rgb);
    float neighborhoodLuminanceSquaredSum = noisyLuminance * noisyLuminance;
    float neighborhoodCount = 1.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const int2 neighbor = clamp(
                int2(pixel) + int2(x, y),
                int2(0, 0),
                int2(g_TraceDimensions) - 1);
            const uint2 neighborFullPixel = MapPixel(
                uint2(neighbor),
                g_TraceDimensions,
                g_FullDimensions);
            const uint neighborMaterial = g_CurrentMaterial.Load(
                int3(neighborFullPixel, 0));
            const float neighborDepth = g_CurrentDepth.Load(
                int3(neighborFullPixel, 0));
            const float3 neighborNormal = SafeNormal(
                g_CurrentNormal.Load(int3(neighborFullPixel, 0)).xyz);
            if (neighborMaterial != currentMaterial ||
                abs(neighborDepth - currentDepth) > g_DepthThreshold ||
                dot(neighborNormal, currentNormal) < g_NormalThreshold)
            {
                continue;
            }
            const float3 value = g_NoisyDiffuse.Load(int3(neighbor, 0)).rgb;
            neighborhoodMinimum = min(neighborhoodMinimum, value);
            neighborhoodMaximum = max(neighborhoodMaximum, value);
            if (x != 0 || y != 0)
            {
                neighborhoodSum += value;
                neighborhoodSquaredSum += value * value;
                const float neighborLuminance = Luminance(value);
                neighborhoodLuminanceSquaredSum += neighborLuminance * neighborLuminance;
                neighborhoodCount += 1.0f;
            }
        }
    }
    const float3 neighborhoodMean = neighborhoodSum / neighborhoodCount;
    const float3 neighborhoodVariance = max(
        neighborhoodSquaredSum / neighborhoodCount -
            neighborhoodMean * neighborhoodMean,
        0.0f);
    const float3 neighborhoodDeviation = sqrt(neighborhoodVariance);
    const float reprojectionPixels = all(isfinite(motion))
        ? length(motion * float2(g_FullDimensions))
        : 0.0f;
    if (g_TemporalConstants.g_ScreenSpaceSignal != 0u)
    {
        // A one-ray SSGI neighborhood can contain only dim observations even
        // when its converged mean includes rare sunlit hits. Raw min/max would
        // erase that energy every such frame. Retain the statistical uncertainty
        // of the accumulated scene-linear illumination instead.
        const float temporalVariance = max(
            previousMoments.y - previousMoments.x * previousMoments.x, 0.0f);
        const float effectiveHistory = clamp(previousLength, 1.0f, 32.0f);
        const float neighborhoodLuminanceMean = Luminance(neighborhoodMean);
        const float neighborhoodLuminanceVariance = max(
            neighborhoodLuminanceSquaredSum / neighborhoodCount -
                neighborhoodLuminanceMean * neighborhoodLuminanceMean, 0.0f);
        const float statisticalDeviation = sqrt(
            neighborhoodLuminanceVariance + temporalVariance / effectiveHistory);
        // Motion does not reduce the uncertainty of sparse SSGI observations.
        // Tightening to 2 sigma during camera motion repeatedly removed valid
        // bounce energy. Keep the statistical interval; geometric reprojection
        // rejects disocclusions, and zero observations still age out stale light.
        const float clampSigma = 3.0f;
        const float minimumLuminance = max(
            neighborhoodLuminanceMean - statisticalDeviation * clampSigma, 0.0f);
        const float maximumLuminance =
            neighborhoodLuminanceMean + statisticalDeviation * clampSigma;
        const float previousLuminance = Luminance(previousDiffuse.rgb);
        if (bValid && bHasObservation && previousLuminance > 1e-6f)
        {
            // Moments measure luminance variance, not RGB variance. Applying
            // the scalar interval in its own space preserves saturated bounce
            // colors and avoids underestimating blue-channel uncertainty.
            previousDiffuse.rgb *= clamp(previousLuminance,
                minimumLuminance, maximumLuminance) / previousLuminance;
        }
    }
    else
    {
        // Preserve the existing RTGI visibility clamp independently of SSGI.
        const float clampSigma = reprojectionPixels > 0.01f
            ? 0.5f
            : max(g_NeighborhoodClampSigma, 0.5f);
        neighborhoodMinimum = max(
            neighborhoodMinimum,
            neighborhoodMean - neighborhoodDeviation * clampSigma);
        neighborhoodMaximum = min(
            neighborhoodMaximum,
            neighborhoodMean + neighborhoodDeviation * clampSigma);
    }
    // Preserve the RTGI rule that all-zero one-sample neighborhoods cannot
    // erase mature history through raw extrema. SSGI instead uses the above
    // statistical interval even for zeros, so stale illumination can decay
    // without throwing away the uncertainty of sparse observations.
    const bool bNeighborhoodHasRadiance = any(
        neighborhoodMaximum > float3(1e-5f, 1e-5f, 1e-5f));
    if (g_TemporalConstants.g_ScreenSpaceSignal == 0u &&
        bValid && bHasObservation && bNeighborhoodHasRadiance)
    {
        previousDiffuse.rgb = clamp(
            previousDiffuse.rgb,
            neighborhoodMinimum,
            neighborhoodMaximum);
    }
    }
    // Missing screen-space observations retain radiance but slowly age its
    // confidence. This prevents a temporarily covered SSGI sample from
    // remaining fully trusted forever after it leaves the observable domain.
    const float retainedHistoryLength = bHasObservation
        ? min(previousLength + observationWeight, maximumHistoryLength)
        : previousLength * 0.98f;
    const float historyLength = bValid
        ? retainedHistoryLength
        : observationWeight;
    const float blend = !bValid
        ? 1.0f
        : bHasObservation
            ? max(
                observationWeight / max(historyLength, observationWeight),
                minimumBlend * observationWeight)
            : 0.0f;
    const float3 temporalDiffuse = !bValid
        ? noisy.rgb
        : bHasObservation
            ? lerp(previousDiffuse.rgb, noisy.rgb, blend)
            : previousDiffuse.rgb;
    const float currentHitDistance = g_HitDistance.Load(int3(pixel, 0));
    const float temporalHitDistance = !bValid
        ? currentHitDistance
        : bHasObservation
            ? lerp(previousHitDistance, currentHitDistance, blend)
            : previousHitDistance;
    const float luminance = Luminance(noisy.rgb);
    const float2 currentMoments = float2(luminance, luminance * luminance);
    const float2 temporalMoments = !bValid
        ? currentMoments
        : bHasObservation
            ? lerp(previousMoments, currentMoments, blend)
            : previousMoments;

    g_HistoryDiffuse[pixel] = float4(temporalDiffuse, 1.0f);
    g_HistoryMoments[pixel] = temporalMoments;
    g_HistoryLength[pixel] = historyLength;
    g_HistoryDepth[pixel] = currentDepth;
    g_HistoryNormal[pixel] = float4(currentNormal, 0.0f);
    g_HistoryMaterial[pixel] = currentMaterial;
    // HistoryLength is now an effective observation count. Low-coverage SSGI
    // regions therefore remain visibly lower confidence instead of reaching
    // full confidence merely because many wall-clock frames elapsed.
    g_TemporalConfidence[pixel] = saturate(historyLength / 32.0f);
    g_HistoryHitDistance[pixel] = isfinite(temporalHitDistance)
        ? max(temporalHitDistance, 0.0f)
        : 0.0f;
}
#endif

#if HISTORY_FIX
struct HistoryFixConstants
{
    uint2 g_Dimensions;
    uint2 g_HistoryFixFullDimensions;
    float g_DepthSigma;
    float g_NormalPower;
    float g_AlbedoSigma;
    float g_RoughnessSigma;
    uint g_HistoryFixOptimizedKernels;
    uint3 g_HistoryFixPadding;
};
HE_DECLARE_PUSH_CONSTANTS(HistoryFixConstants, g_HistoryFixConstants, space3);
#define g_Dimensions g_HistoryFixConstants.g_Dimensions
#define g_DepthSigma g_HistoryFixConstants.g_DepthSigma
#define g_NormalPower g_HistoryFixConstants.g_NormalPower
#define g_HistoryFixFullDimensions g_HistoryFixConstants.g_HistoryFixFullDimensions
#define g_AlbedoSigma g_HistoryFixConstants.g_AlbedoSigma
#define g_RoughnessSigma g_HistoryFixConstants.g_RoughnessSigma
Texture2D<float4> g_TemporalDiffuse : register(t0, space3);
Texture2D<float> g_HistoryLengthInput : register(t1, space3);
Texture2D<float> g_HistoryDepthInput : register(t2, space3);
Texture2D<float4> g_HistoryNormalInput : register(t3, space3);
Texture2D<uint> g_HistoryMaterialInput : register(t4, space3);
Texture2D<float4> g_HistoryFixBaseColorMetalness : register(t5, space3);
Texture2D<float4> g_HistoryFixEmissionModel : register(t7, space3);
Texture2D<float4> g_HistoryFixNormalRoughness : register(t6, space3);
RWTexture2D<float4> g_HistoryFixedDiffuse : register(u0, space3);

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= g_Dimensions))
        return;
    const float historyLength = g_HistoryLengthInput.Load(int3(pixel, 0));
    const float4 center = g_TemporalDiffuse.Load(int3(pixel, 0));
    if (historyLength > 1.0f)
    {
        g_HistoryFixedDiffuse[pixel] = center;
        return;
    }
    const uint centerMaterial = g_HistoryMaterialInput.Load(int3(pixel, 0));
    [branch]
    if (g_HistoryFixConstants.g_HistoryFixOptimizedKernels != 0u && centerMaterial == c_InvalidMaterial)
    {
        // Every original neighbor fails the invalid-material check, leaving
        // exactly the center fallback. Avoid the redundant nine-tap gather.
        g_HistoryFixedDiffuse[pixel] = float4(center.rgb, 1.0f);
        return;
    }
    const float centerDepth = g_HistoryDepthInput.Load(int3(pixel, 0));
    const float3 centerNormal = SafeNormal(
        g_HistoryNormalInput.Load(int3(pixel, 0)).xyz);
    const uint2 centerFullPixel = MapPixel(
        pixel,
        g_Dimensions,
        g_HistoryFixFullDimensions);
    const float3 centerAlbedo = DiffuseAlbedo(
        g_HistoryFixBaseColorMetalness.Load(int3(centerFullPixel, 0)),
        g_HistoryFixEmissionModel.Load(int3(centerFullPixel, 0)).a);
    const float centerRoughness = g_HistoryFixNormalRoughness.Load(
        int3(centerFullPixel, 0)).w;
    float3 sum = 0.0f;
    float weightSum = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const int2 neighbor = clamp(
                int2(pixel) + int2(x, y),
                int2(0, 0),
                int2(g_Dimensions) - 1);
            const float depth = g_HistoryDepthInput.Load(int3(neighbor, 0));
            const float3 normal = SafeNormal(
                g_HistoryNormalInput.Load(int3(neighbor, 0)).xyz);
            const uint material = g_HistoryMaterialInput.Load(
                int3(neighbor, 0));
            if (material != centerMaterial || material == c_InvalidMaterial)
                continue;
            const uint2 neighborFullPixel = MapPixel(
                uint2(neighbor),
                g_Dimensions,
                g_HistoryFixFullDimensions);
            const float3 albedo = DiffuseAlbedo(
                g_HistoryFixBaseColorMetalness.Load(
                    int3(neighborFullPixel, 0)),
                g_HistoryFixEmissionModel.Load(int3(neighborFullPixel, 0)).a);
            const float roughness = g_HistoryFixNormalRoughness.Load(
                int3(neighborFullPixel, 0)).w;
            const float depthExponent =
                abs(depth - centerDepth) / max(g_DepthSigma, 1e-6f);
            const float normalWeight = pow(
                max(dot(centerNormal, normal), 0.0f),
                g_NormalPower);
            const float albedoExponent =
                length(albedo - centerAlbedo) / max(g_AlbedoSigma, 1e-6f);
            const float roughnessExponent =
                abs(roughness - centerRoughness) / max(g_RoughnessSigma, 1e-6f);
            const float weight = normalWeight *
                exp(-(depthExponent + albedoExponent + roughnessExponent));
            sum += g_TemporalDiffuse.Load(int3(neighbor, 0)).rgb * weight;
            weightSum += weight;
        }
    }
    g_HistoryFixedDiffuse[pixel] = float4(
        weightSum > 0.0f ? sum / weightSum : center.rgb,
        1.0f);
}
#endif

#if VARIANCE
struct VarianceConstants
{
    uint2 g_Dimensions;
    float2 g_VariancePadding;
};
HE_DECLARE_PUSH_CONSTANTS(VarianceConstants, g_VarianceConstants, space3);
#define g_Dimensions g_VarianceConstants.g_Dimensions
#define g_VariancePadding g_VarianceConstants.g_VariancePadding
Texture2D<float2> g_Moments : register(t0, space3);
Texture2D<float> g_VarianceHistoryLength : register(t1, space3);
RWTexture2D<float> g_Variance : register(u0, space3);

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= g_Dimensions))
        return;
    const float2 moments = g_Moments.Load(int3(pixel, 0));
    const float historyLength = g_VarianceHistoryLength.Load(int3(pixel, 0));
    const float variance = max(moments.y - moments.x * moments.x, 0.0f);
    g_Variance[pixel] = variance * (historyLength < 4.0f ? 4.0f : 1.0f);
}
#endif

#if UNCERTAINTY
struct UncertaintyConstants
{
    uint2 g_Dimensions;
    float g_PhiDepth;
    float g_PhiNormal;
};
HE_DECLARE_PUSH_CONSTANTS(UncertaintyConstants, g_UncertaintyConstants, space3);
#define g_Dimensions g_UncertaintyConstants.g_Dimensions
#define g_PhiDepth g_UncertaintyConstants.g_PhiDepth
#define g_PhiNormal g_UncertaintyConstants.g_PhiNormal
Texture2D<float> g_UncertaintyVariance : register(t0, space3);
Texture2D<float> g_UncertaintyHistoryLength : register(t1, space3);
Texture2D<float> g_UncertaintyDepth : register(t2, space3);
Texture2D<float4> g_UncertaintyNormal : register(t3, space3);
Texture2D<uint> g_UncertaintyMaterial : register(t4, space3);
RWTexture2D<float> g_UncertaintyOutput : register(u0, space3);

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= g_Dimensions)) return;
    const uint centerMaterial = g_UncertaintyMaterial.Load(int3(pixel, 0));
    const float centerDepth = g_UncertaintyDepth.Load(int3(pixel, 0));
    const float3 centerNormal = SafeNormal(g_UncertaintyNormal.Load(int3(pixel, 0)).xyz);
    // Same footprint, tap order and full-float arithmetic as the reference.
    float uncertaintySum = 0.0f;
    float uncertaintyWeight = 0.0f;
    [unroll]
    for (int vy = -1; vy <= 1; ++vy)
    {
        [unroll]
        for (int vx = -1; vx <= 1; ++vx)
        {
            const int2 neighbor = int2(pixel) + int2(vx, vy);
            if (any(neighbor < 0) || any(neighbor >= int2(g_Dimensions))) continue;
            if (g_UncertaintyMaterial.Load(int3(neighbor, 0)) != centerMaterial) continue;
            const float depth = g_UncertaintyDepth.Load(int3(neighbor, 0));
            const float3 normal = SafeNormal(g_UncertaintyNormal.Load(int3(neighbor, 0)).xyz);
            const float weight = (vx == 0 ? 2.0f : 1.0f) * (vy == 0 ? 2.0f : 1.0f) *
                exp(-abs(depth - centerDepth) /
                    max(g_PhiDepth * max(abs(centerDepth), 1e-3f), 1e-6f)) *
                pow(max(dot(centerNormal, normal), 0.0f), g_PhiNormal);
            const float length = clamp(g_UncertaintyHistoryLength.Load(int3(neighbor, 0)), 1.0f, 32.0f);
            uncertaintySum += weight * max(g_UncertaintyVariance.Load(int3(neighbor, 0)), 0.0f) / length;
            uncertaintyWeight += weight;
        }
    }
    g_UncertaintyOutput[pixel] = uncertaintySum / max(uncertaintyWeight, 1e-6f);
}
#endif

#if ATROUS || ATROUS_CACHED
struct AtrousConstants
{
    uint2 g_Dimensions;
    uint2 g_AtrousFullDimensions;
    uint g_StepWidth;
    float g_PhiDepth;
    float g_PhiNormal;
    float g_PhiLuminance;
    float g_PhiAlbedo;
    float g_PhiRoughness;
    uint g_AtrousOptimizedKernels;
    uint g_AtrousScreenSpaceSignal;
    float g_DiffuseHitDistanceWeight;
    uint g_AtrousUseSharedUncertainty;
    uint2 g_AtrousUncertaintyPadding;
};
HE_DECLARE_PUSH_CONSTANTS(AtrousConstants, g_AtrousConstants, space3);
#define g_Dimensions g_AtrousConstants.g_Dimensions
#define g_StepWidth g_AtrousConstants.g_StepWidth
#define g_PhiDepth g_AtrousConstants.g_PhiDepth
#define g_PhiNormal g_AtrousConstants.g_PhiNormal
#define g_PhiLuminance g_AtrousConstants.g_PhiLuminance
#define g_AtrousFullDimensions g_AtrousConstants.g_AtrousFullDimensions
#define g_PhiAlbedo g_AtrousConstants.g_PhiAlbedo
#define g_PhiRoughness g_AtrousConstants.g_PhiRoughness
#define g_AtrousPadding g_AtrousConstants.g_AtrousPadding
Texture2D<float4> g_AtrousInput : register(t0, space3);
Texture2D<float> g_AtrousVariance : register(t1, space3);
Texture2D<float> g_AtrousDepth : register(t2, space3);
Texture2D<float4> g_AtrousNormal : register(t3, space3);
Texture2D<float> g_AtrousHitDistance : register(t4, space3);
Texture2D<uint> g_AtrousMaterial : register(t5, space3);
Texture2D<float4> g_AtrousBaseColorMetalness : register(t6, space3);
Texture2D<float4> g_AtrousEmissionModel : register(t10, space3);
Texture2D<float4> g_AtrousNormalRoughness : register(t7, space3);
Texture2D<float> g_AtrousConfidence : register(t8, space3);
Texture2D<float> g_AtrousHistoryLength : register(t9, space3);
RWTexture2D<float4> g_AtrousOutput : register(u0, space3);

#if ATROUS_CACHED
// Separate variant: the reference kernel reserves no shared storage. Only
// Steps 1/2 use contiguous 12x12/16x16 tiles. Step 4 groups outputs
// with the same residue modulo the step, so a 12x12 tile covers all taps
// without a large shared-memory halo. Dispatch padding covers partial tiles.
// Keep full float precision and the original 25 taps / accumulation order.
struct AtrousCachedTap
{
    float4 ValueDepth;
    float4 NormalHit;
    float4 AlbedoRoughness;
    float2 ConfidenceHistory;
    uint Material;
};
groupshared AtrousCachedTap g_AtrousTile[256];

void FillAtrousTile(uint2 groupId, uint2 groupThreadId)
{
    const bool strided = g_StepWidth > 2u;
    const uint tileWidth = strided ? 12u : 8u + 4u * g_StepWidth;
    const uint stride = strided ? g_StepWidth : 1u;
    const uint2 origin = strided
        ? (groupId / g_StepWidth) * (8u * g_StepWidth) + groupId % g_StepWidth
        : groupId * 8u;
    const uint threadIndex = groupThreadId.y * 8u + groupThreadId.x;
    for (uint index = threadIndex; index < tileWidth * tileWidth; index += 64u)
    {
        const int2 pixel = clamp(int2(origin) +
            int2(index % tileWidth, index / tileWidth) * int(stride) - int(2u * g_StepWidth),
            int2(0, 0), int2(g_Dimensions) - 1);
        AtrousCachedTap tap = (AtrousCachedTap)0;
        tap.Material = g_AtrousMaterial.Load(int3(pixel, 0));
        // Invalid neighbors are always rejected by the original filter.
        if (tap.Material != c_InvalidMaterial)
        {
            const uint2 fullPixel = MapPixel(uint2(pixel), g_Dimensions, g_AtrousFullDimensions);
            tap.ValueDepth = float4(g_AtrousInput.Load(int3(pixel, 0)).rgb,
                g_AtrousDepth.Load(int3(pixel, 0)));
            tap.NormalHit = float4(SafeNormal(g_AtrousNormal.Load(int3(pixel, 0)).xyz),
                g_AtrousHitDistance.Load(int3(pixel, 0)));
            tap.AlbedoRoughness = float4(DiffuseAlbedo(
                g_AtrousBaseColorMetalness.Load(int3(fullPixel, 0)),
                g_AtrousEmissionModel.Load(int3(fullPixel, 0)).a),
                g_AtrousNormalRoughness.Load(int3(fullPixel, 0)).w);
            tap.ConfidenceHistory = float2(g_AtrousConfidence.Load(int3(pixel, 0)),
                g_AtrousHistoryLength.Load(int3(pixel, 0)));
        }
        g_AtrousTile[index] = tap;
    }
    GroupMemoryBarrierWithGroupSync();
}
#endif

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID,
    uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
#if ATROUS_CACHED
    // Edge groups must participate before out-of-bounds/background returns.
    FillAtrousTile(groupId.xy, groupThreadId.xy);
    const uint2 pixel = g_StepWidth > 2u
        ? (groupId.xy / g_StepWidth) * (8u * g_StepWidth) +
            groupId.xy % g_StepWidth + groupThreadId.xy * g_StepWidth
        : dispatchThreadId.xy;
#else
    const uint2 pixel = dispatchThreadId.xy;
#endif
    if (any(pixel >= g_Dimensions))
        return;
    const float4 center = g_AtrousInput.Load(int3(pixel, 0));
    const uint centerMaterial = g_AtrousMaterial.Load(int3(pixel, 0));
    [branch]
    if (g_AtrousConstants.g_AtrousOptimizedKernels != 0u && centerMaterial == c_InvalidMaterial)
    {
        // Invalid center material can never accept any of the 25 neighbors.
        g_AtrousOutput[pixel] = float4(center.rgb, 1.0f);
        return;
    }
    const float centerDepth = g_AtrousDepth.Load(int3(pixel, 0));
    const float3 centerNormal = SafeNormal(
        g_AtrousNormal.Load(int3(pixel, 0)).xyz);
    const float centerHit = g_AtrousHitDistance.Load(int3(pixel, 0));
    const uint2 centerFullPixel = MapPixel(
        pixel,
        g_Dimensions,
        g_AtrousFullDimensions);
    const float3 centerAlbedo = DiffuseAlbedo(
        g_AtrousBaseColorMetalness.Load(int3(centerFullPixel, 0)),
        g_AtrousEmissionModel.Load(int3(centerFullPixel, 0)).a);
    const float centerRoughness = g_AtrousNormalRoughness.Load(
        int3(centerFullPixel, 0)).w;
    const float centerConfidence = g_AtrousConfidence.Load(int3(pixel, 0));
    const float centerHistoryLength = g_AtrousHistoryLength.Load(
        int3(pixel, 0));
    // The temporal moments describe per-observation variance. Once history is
    // mature, spatial filtering must use uncertainty of the accumulated mean;
    // otherwise a permanently wide kernel leaks bright direct regions into
    // stable indirect shadows. Cap the divisor to keep enough early-frame
    // denoising while converging toward the temporal center.
    const float effectiveHistory = clamp(centerHistoryLength, 1.0f, 32.0f);
    float varianceOfMean = max(
        g_AtrousVariance.Load(int3(pixel, 0)),
        0.0f) / effectiveHistory;
    if (g_AtrousConstants.g_AtrousUseSharedUncertainty != 0u)
    {
        // t1 holds prefiltered variance-of-mean, not the debug variance.
        varianceOfMean = g_AtrousVariance.Load(int3(pixel, 0));
    }
    else if (g_AtrousConstants.g_AtrousScreenSpaceSignal != 0u)
    {
        // SVGF (HPG 2017, section 4.4): sparse transport underestimates local
        // variance. Pre-filter uncertainty ONLY for the luminance edge stop;
        // leave radiance, moments and zero observations untouched. Restrict
        // the 3x3 footprint to the same material/normal/depth neighborhood so
        // a bright surface across a wall cannot open the luminance gate.
        float uncertaintySum = 0.0f;
        float uncertaintyWeight = 0.0f;
        [unroll]
        for (int vy = -1; vy <= 1; ++vy)
        {
            [unroll]
            for (int vx = -1; vx <= 1; ++vx)
            {
                const int2 neighbor = int2(pixel) + int2(vx, vy);
                if (any(neighbor < 0) || any(neighbor >= int2(g_Dimensions))) continue;
                if (g_AtrousMaterial.Load(int3(neighbor, 0)) != centerMaterial) continue;
                const float depth = g_AtrousDepth.Load(int3(neighbor, 0));
                const float3 normal = SafeNormal(g_AtrousNormal.Load(int3(neighbor, 0)).xyz);
                const float weight = (vx == 0 ? 2.0f : 1.0f) * (vy == 0 ? 2.0f : 1.0f) *
                    exp(-abs(depth - centerDepth) /
                        max(g_PhiDepth * max(abs(centerDepth), 1e-3f), 1e-6f)) *
                    pow(max(dot(centerNormal, normal), 0.0f), g_PhiNormal);
                const float length = clamp(g_AtrousHistoryLength.Load(int3(neighbor, 0)), 1.0f, 32.0f);
                uncertaintySum += weight * max(g_AtrousVariance.Load(int3(neighbor, 0)), 0.0f) / length;
                uncertaintyWeight += weight;
            }
        }
        varianceOfMean = uncertaintySum / max(uncertaintyWeight, 1e-6f);
    }
    const float sigmaLuminance = max(
        sqrt(varianceOfMean) * g_PhiLuminance,
        max(Luminance(center.rgb) * 0.02f, 1e-4f));
    // Uniform branch preserves the original arithmetic at default weight 1.
    // Receiver geometry/material/luminance gates and normalized radiance stay intact.
    const bool bRelaxDiffuseHitGuide = g_AtrousConstants.g_AtrousScreenSpaceSignal != 0u &&
        g_AtrousConstants.g_DiffuseHitDistanceWeight < 1.0f;
    const float kernel[5] = { 1.0f, 4.0f, 6.0f, 4.0f, 1.0f };
    float3 sum = 0.0f;
    float weightSum = 0.0f;
    [loop]
    for (int y = -2; y <= 2; ++y)
    {
        [loop]
        for (int x = -2; x <= 2; ++x)
        {
            const int2 neighbor = clamp(
                int2(pixel) + int2(x, y) * int(g_StepWidth),
                int2(0, 0),
                int2(g_Dimensions) - 1);
#if ATROUS_CACHED
            const uint tileWidth = g_StepWidth > 2u ? 12u : 8u + 4u * g_StepWidth;
            const uint tileStep = g_StepWidth > 2u ? 1u : g_StepWidth;
            const uint2 tilePixel = groupThreadId.xy + uint2(x + 2, y + 2) * tileStep;
            const AtrousCachedTap tap = g_AtrousTile[tilePixel.y * tileWidth + tilePixel.x];
            const float3 value = tap.ValueDepth.rgb;
            const float depth = tap.ValueDepth.w;
            const float3 normal = tap.NormalHit.xyz;
            const float hit = tap.NormalHit.w;
            const uint material = tap.Material;
#else
            const float3 value = g_AtrousInput.Load(int3(neighbor, 0)).rgb;
            const float depth = g_AtrousDepth.Load(int3(neighbor, 0));
            const float3 normal = SafeNormal(
                g_AtrousNormal.Load(int3(neighbor, 0)).xyz);
            const float hit = g_AtrousHitDistance.Load(int3(neighbor, 0));
            const uint material = g_AtrousMaterial.Load(int3(neighbor, 0));
#endif
            if (material != centerMaterial || material == c_InvalidMaterial)
                continue;
#if ATROUS_CACHED
            const float3 albedo = tap.AlbedoRoughness.rgb;
            const float roughness = tap.AlbedoRoughness.w;
            const float confidence = tap.ConfidenceHistory.x;
            const float historyLength = tap.ConfidenceHistory.y;
#else
            const uint2 neighborFullPixel = MapPixel(
                uint2(neighbor),
                g_Dimensions,
                g_AtrousFullDimensions);
            const float3 albedo = DiffuseAlbedo(
                g_AtrousBaseColorMetalness.Load(
                    int3(neighborFullPixel, 0)),
                g_AtrousEmissionModel.Load(int3(neighborFullPixel, 0)).a);
            const float roughness = g_AtrousNormalRoughness.Load(
                int3(neighborFullPixel, 0)).w;
            const float confidence = g_AtrousConfidence.Load(
                int3(neighbor, 0));
            const float historyLength = g_AtrousHistoryLength.Load(
                int3(neighbor, 0));
#endif
            const float spatialWeight = kernel[x + 2] * kernel[y + 2];
            const float normalWeight = pow(
                max(dot(centerNormal, normal), 0.0f),
                g_PhiNormal);
            // Zero hit distance means no observable secondary surface for
            // SSGI, not a geometric discontinuity. Apply the hit-distance
            // guide only when both samples own a real secondary hit so valid
            // neighboring illumination can fill low-coverage screen regions.
            const bool bCenterHasHit = centerHit > 1e-5f;
            const bool bNeighborHasHit = hit > 1e-5f;
            const float confidenceWeight = lerp(
                0.25f,
                1.0f,
                saturate(min(centerConfidence, confidence)));
            const float historyWeight = lerp(
                0.25f,
                1.0f,
                saturate(min(centerHistoryLength, historyLength) / 8.0f));
            float weight;
            [branch]
            if (g_AtrousConstants.g_AtrousOptimizedKernels != 0u)
            {
                // Products of five positive exponentials equal the exponential
                // of their summed exponents. Same guides/taps/kernel; only
                // floating-point evaluation order changes versus the baseline.
                const float depthExponent = abs(depth - centerDepth) /
                    max(g_PhiDepth * max(abs(centerDepth), 1e-3f), 1e-6f);
                const float luminanceExponent = abs(Luminance(value) - Luminance(center.rgb)) / sigmaLuminance;
                float hitExponent = bCenterHasHit && bNeighborHasHit
                    ? abs(hit - centerHit) / max(abs(centerHit) * 0.1f + 1e-3f, 1e-3f) : 0.0f;
                if (bRelaxDiffuseHitGuide)
                    hitExponent *= g_AtrousConstants.g_DiffuseHitDistanceWeight;
                const float albedoExponent = length(albedo - centerAlbedo) / max(g_PhiAlbedo, 1e-6f);
                const float roughnessExponent = abs(roughness - centerRoughness) / max(g_PhiRoughness, 1e-6f);
                weight = spatialWeight * normalWeight *
                    exp(-(depthExponent + luminanceExponent + hitExponent + albedoExponent + roughnessExponent)) *
                    confidenceWeight * historyWeight;
            }
            else
            {
                const float depthWeight = exp(
                    -abs(depth - centerDepth) /
                    max(g_PhiDepth * max(abs(centerDepth), 1e-3f), 1e-6f));
                const float luminanceWeight = exp(
                    -abs(Luminance(value) - Luminance(center.rgb)) / sigmaLuminance);
                float hitWeight = bCenterHasHit && bNeighborHasHit
                    ? exp(-abs(hit - centerHit) / max(abs(centerHit) * 0.1f + 1e-3f, 1e-3f)) : 1.0f;
                if (bRelaxDiffuseHitGuide && bCenterHasHit && bNeighborHasHit)
                    hitWeight = exp(-g_AtrousConstants.g_DiffuseHitDistanceWeight *
                        abs(hit - centerHit) / max(abs(centerHit) * 0.1f + 1e-3f, 1e-3f));
                const float albedoWeight = exp(
                    -length(albedo - centerAlbedo) / max(g_PhiAlbedo, 1e-6f));
                const float roughnessWeight = exp(
                    -abs(roughness - centerRoughness) / max(g_PhiRoughness, 1e-6f));
                weight = spatialWeight * depthWeight * normalWeight * luminanceWeight * hitWeight *
                    albedoWeight * roughnessWeight * confidenceWeight * historyWeight;
            }
            sum += value * weight;
            weightSum += weight;
        }
    }
    g_AtrousOutput[pixel] = float4(
        weightSum > 0.0f ? sum / weightSum : center.rgb,
        1.0f);
}
#endif

#if UPSAMPLE
struct UpsampleConstants
{
    uint2 g_LowDimensions;
    uint2 g_UpsampleFullDimensions;
    float g_UpsampleDepthSigma;
    float g_UpsampleNormalPower;
    float g_UpsampleAlbedoSigma;
    float g_UpsampleRoughnessSigma;
};
HE_DECLARE_PUSH_CONSTANTS(UpsampleConstants, g_UpsampleConstants, space3);
#define g_LowDimensions g_UpsampleConstants.g_LowDimensions
#define g_UpsampleFullDimensions \
    g_UpsampleConstants.g_UpsampleFullDimensions
#define g_UpsampleDepthSigma g_UpsampleConstants.g_UpsampleDepthSigma
#define g_UpsampleNormalPower g_UpsampleConstants.g_UpsampleNormalPower
#define g_UpsampleAlbedoSigma g_UpsampleConstants.g_UpsampleAlbedoSigma
#define g_UpsampleRoughnessSigma g_UpsampleConstants.g_UpsampleRoughnessSigma
Texture2D<float4> g_LowDiffuse : register(t0, space3);
Texture2D<float> g_FullDepth : register(t1, space3);
Texture2D<float4> g_FullNormal : register(t2, space3);
Texture2D<float> g_LowDepth : register(t3, space3);
Texture2D<float4> g_LowNormal : register(t4, space3);
Texture2D<float4> g_UpsampleBaseColorMetalness : register(t5, space3);
Texture2D<float4> g_UpsampleEmissionModel : register(t11, space3);
Texture2D<uint> g_UpsampleFullMaterial : register(t6, space3);
Texture2D<uint> g_UpsampleLowMaterial : register(t7, space3);
Texture2D<float> g_UpsampleLowHitDistance : register(t8, space3);
Texture2D<float> g_UpsampleLowConfidence : register(t9, space3);
Texture2D<float> g_UpsampleLowCoverage : register(t10, space3);
RWTexture2D<float4> g_FullDiffuse : register(u0, space3);
RWTexture2D<float> g_FullHitDistance : register(u1, space3);
RWTexture2D<float> g_FullConfidence : register(u2, space3);
RWTexture2D<float> g_FullCoverage : register(u3, space3);

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= g_UpsampleFullDimensions))
        return;
    const float centerDepth = g_FullDepth.Load(int3(pixel, 0));
    const float4 centerNormalRoughness = g_FullNormal.Load(int3(pixel, 0));
    const float3 centerNormal = SafeNormal(centerNormalRoughness.xyz);
    const float4 centerBaseColorMetalness =
        g_UpsampleBaseColorMetalness.Load(int3(pixel, 0));
    const float3 centerAlbedo = DiffuseAlbedo(centerBaseColorMetalness,
        g_UpsampleEmissionModel.Load(int3(pixel, 0)).a);
    const float centerRoughness = centerNormalRoughness.w;
    const uint centerMaterial = g_UpsampleFullMaterial.Load(int3(pixel, 0));
    [branch]
    if (centerMaterial == c_InvalidMaterial || !isfinite(centerDepth) ||
        centerDepth >= 1.0f || !any(centerAlbedo > 0.0f))
    {
        g_FullDiffuse[pixel] = 0.0f;
        g_FullHitDistance[pixel] = 0.0f;
        g_FullConfidence[pixel] = 0.0f;
        g_FullCoverage[pixel] = 0.0f;
        return;
    }
    const float2 lowPosition = (float2(pixel) + 0.5f) *
        float2(g_LowDimensions) / float2(g_UpsampleFullDimensions) - 0.5f;
    const int2 lowBase = int2(floor(lowPosition));
    float3 sum = 0.0f;
    float hitSum = 0.0f;
    float confidenceSum = 0.0f;
    float coverageSum = 0.0f;
    float weightSum = 0.0f;
    const int2 fallbackPixel = clamp(
        int2(round(lowPosition)),
        int2(0, 0),
        int2(g_LowDimensions) - 1);
    const float centerHit = g_UpsampleLowHitDistance.Load(
        int3(fallbackPixel, 0));
    [unroll]
    for (int y = 0; y <= 1; ++y)
    {
        [unroll]
        for (int x = 0; x <= 1; ++x)
        {
            const int2 samplePixel = clamp(
                lowBase + int2(x, y),
                int2(0, 0),
                int2(g_LowDimensions) - 1);
            const float depth = g_LowDepth.Load(int3(samplePixel, 0));
            const float3 normal = SafeNormal(
                g_LowNormal.Load(int3(samplePixel, 0)).xyz);
            const uint material = g_UpsampleLowMaterial.Load(
                int3(samplePixel, 0));
            const float hit = g_UpsampleLowHitDistance.Load(
                int3(samplePixel, 0));
            const float confidence = g_UpsampleLowConfidence.Load(
                int3(samplePixel, 0));
            const float coverage = g_UpsampleLowCoverage.Load(
                int3(samplePixel, 0));
            if (material != centerMaterial || material == c_InvalidMaterial ||
                confidence <= 0.0f)
            {
                continue;
            }
            const uint2 sampleFullPixel = MapPixel(
                uint2(samplePixel),
                g_LowDimensions,
                g_UpsampleFullDimensions);
            const float3 albedo = DiffuseAlbedo(
                g_UpsampleBaseColorMetalness.Load(
                    int3(sampleFullPixel, 0)),
                g_UpsampleEmissionModel.Load(int3(sampleFullPixel, 0)).a);
            const float roughness = g_FullNormal.Load(
                int3(sampleFullPixel, 0)).w;
            const float2 delta = abs(float2(samplePixel) - lowPosition);
            const float bilinearWeight =
                max(1.0f - delta.x, 0.0f) * max(1.0f - delta.y, 0.0f);
            const float depthExponent =
                abs(depth - centerDepth) / max(g_UpsampleDepthSigma, 1e-6f);
            const float normalWeight = pow(
                max(dot(centerNormal, normal), 0.0f),
                g_UpsampleNormalPower);
            const float albedoExponent = length(albedo - centerAlbedo) /
                max(g_UpsampleAlbedoSigma, 1e-6f);
            const float roughnessExponent = abs(roughness - centerRoughness) /
                max(g_UpsampleRoughnessSigma, 1e-6f);
            // A zero SSGI hit distance means no observed secondary surface.
            // It must not reject illuminated neighbors during reconstruction.
            const float hitExponent = centerHit > 1e-5f && hit > 1e-5f
                ? abs(hit - centerHit) /
                    max(abs(centerHit) * 0.1f + 1e-3f, 1e-3f)
                : 0.0f;
            const float weight = bilinearWeight * normalWeight * confidence *
                exp(-(depthExponent + albedoExponent + roughnessExponent + hitExponent));
            sum += g_LowDiffuse.Load(int3(samplePixel, 0)).rgb * weight;
            hitSum += hit * weight;
            confidenceSum += confidence * weight;
            coverageSum += coverage * weight;
            weightSum += weight;
        }
    }
    [branch]
    if (weightSum > 0.0f)
    {
        const float inverseWeight = rcp(weightSum);
        g_FullDiffuse[pixel] = float4(sum * inverseWeight, 1.0f);
        g_FullHitDistance[pixel] = hitSum * inverseWeight;
        g_FullConfidence[pixel] = saturate(confidenceSum * inverseWeight);
        g_FullCoverage[pixel] = saturate(coverageSum * inverseWeight);
        return;
    }
    const float3 fallback = g_LowDiffuse.Load(
        int3(fallbackPixel, 0)).rgb;
    const float fallbackHit = g_UpsampleLowHitDistance.Load(
        int3(fallbackPixel, 0));
    const float fallbackConfidence = g_UpsampleLowConfidence.Load(
        int3(fallbackPixel, 0));
    const float fallbackCoverage = g_UpsampleLowCoverage.Load(
        int3(fallbackPixel, 0));
    const uint fallbackMaterial = g_UpsampleLowMaterial.Load(
        int3(fallbackPixel, 0));
    const float fallbackDepth = g_LowDepth.Load(int3(fallbackPixel, 0));
    const float3 fallbackNormal = SafeNormal(
        g_LowNormal.Load(int3(fallbackPixel, 0)).xyz);
    // A nearest neighbor on another surface is not a reconstruction fallback.
    // This commonly happens on thin silhouettes missing from a half-res trace.
    const bool bFallbackValid = fallbackMaterial == centerMaterial &&
        isfinite(fallbackDepth) && fallbackDepth < 1.0f &&
        abs(fallbackDepth - centerDepth) <= 4.0f * g_UpsampleDepthSigma &&
        dot(fallbackNormal, centerNormal) >= 0.8f;
    g_FullDiffuse[pixel] = float4(
        bFallbackValid ? fallback : 0.0f,
        1.0f);
    g_FullHitDistance[pixel] = bFallbackValid ? fallbackHit : 0.0f;
    g_FullConfidence[pixel] = bFallbackValid ? saturate(fallbackConfidence) : 0.0f;
    g_FullCoverage[pixel] = bFallbackValid ? saturate(fallbackCoverage) : 0.0f;
}
#endif

#if REMODULATE
struct RemodulateConstants
{
    uint2 g_RemodulateDimensions;
    float g_RemodulateAlbedoEpsilon;
    uint g_RemodulateRasterSurface;
};
HE_DECLARE_PUSH_CONSTANTS(
    RemodulateConstants,
    g_RemodulateConstants,
    space3);
Texture2D<float4> g_FilteredIllumination : register(t0, space3);
Texture2D<float4> g_RemodulateBaseColorMetalness : register(t1, space3);
Texture2D<float4> g_RemodulateEmissive : register(t2, space3);
RWTexture2D<float4> g_RemodulatedDiffuse : register(u0, space3);

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= g_RemodulateConstants.g_RemodulateDimensions))
        return;
    const float4 baseColorMetalness =
        g_RemodulateBaseColorMetalness.Load(int3(pixel, 0));
    const float4 emissionModel = g_RemodulateEmissive.Load(int3(pixel, 0));
    const float3 emissive = max(emissionModel.rgb, 0.0f);
    const float3 albedo = DiffuseAlbedo(baseColorMetalness, emissionModel.a);
    const float epsilon = max(
        g_RemodulateConstants.g_RemodulateAlbedoEpsilon,
        1e-6f);
    const bool bDiffuse = baseColorMetalness.w < 1.0f &&
        any(albedo > 0.0f);
    const bool bEmissive = g_RemodulateConstants.g_RemodulateRasterSurface == 0u && any(emissive > epsilon);
    const float3 radiance = bDiffuse && !bEmissive
        ? max(g_FilteredIllumination.Load(int3(pixel, 0)).rgb, 0.0f) * albedo
        : 0.0f;
    g_RemodulatedDiffuse[pixel] = float4(
        all(isfinite(radiance)) ? radiance : 0.0f,
        bDiffuse && !bEmissive ? 1.0f : 0.0f);
}
#endif

#if !DEMODULATE && !TEMPORAL && !HISTORY_FIX && !VARIANCE && !UNCERTAINTY && !ATROUS && !ATROUS_CACHED && !UPSAMPLE && !REMODULATE
[numthreads(1, 1, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x == 0xffffffffu)
        return;
}
#endif
