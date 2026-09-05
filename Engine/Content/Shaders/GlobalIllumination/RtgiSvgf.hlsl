#pragma htype cs CS
#pragma hvariant DEMODULATE
#pragma hvariant TEMPORAL
#pragma hvariant HISTORY_FIX
#pragma hvariant VARIANCE
#pragma hvariant ATROUS
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

float3 DiffuseAlbedo(float4 baseColorMetalness)
{
    return max(baseColorMetalness.rgb, 0.0f) *
        (1.0f - saturate(baseColorMetalness.w));
}

#if DEMODULATE
struct DemodulateConstants
{
    uint2 g_DemodulateTraceDimensions;
    uint2 g_DemodulateFullDimensions;
    float g_AlbedoEpsilon;
    float3 g_DemodulatePadding;
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
    const float3 emissive = max(
        g_DemodulateEmissive.Load(int3(fullPixel, 0)).rgb,
        0.0f);
    const float3 albedo = DiffuseAlbedo(baseColorMetalness);
    const float epsilon = max(
        g_DemodulateConstants.g_AlbedoEpsilon,
        1e-6f);
    const bool bDiffuse = baseColorMetalness.w < 1.0f &&
        any(albedo > 0.0f);
    const bool bEmissive = any(emissive > epsilon);
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
    float2 g_TemporalPadding;
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

struct ReprojectedHistory
{
    float4 Diffuse;
    float2 Moments;
    float Length;
    float HitDistance;
    float ValidWeight;
};

bool IsHistoryTapValid(
    int2 tapPixel,
    float expectedPreviousViewDepth,
    float expectedPreviousDepth,
    float3 currentNormal,
    uint currentMaterial,
    uint2 currentSurfaceId)
{
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
    return previousMaterial == currentMaterial &&
        all(previousSurfaceId == currentSurfaceId) &&
        isfinite(previousDepth) &&
        isfinite(expectedPreviousDepth) &&
        isfinite(previousViewDepth) && previousViewDepth > 0.0f &&
        isfinite(expectedPreviousViewDepth) &&
        expectedPreviousViewDepth > 0.0f &&
        abs(previousViewDepth - expectedPreviousViewDepth) <=
            viewDepthTolerance &&
        abs(previousDepth - expectedPreviousDepth) <= g_DepthThreshold &&
        dot(previousNormal, currentNormal) >= g_NormalThreshold;
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
            if (weight <= 0.0f || !IsHistoryTapValid(
                    tapPixel,
                    expectedPreviousViewDepth,
                    expectedPreviousDepth,
                    currentNormal,
                    currentMaterial,
                    currentSurfaceId))
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
        // A minority-valid bilinear footprint is a disocclusion boundary, not
        // a stable history sample. Reject it instead of normalizing a tiny tap
        // into a full-strength contribution that can walk across silhouettes.
        bValid = reprojected.ValidWeight >= 0.5f;
        previousDiffuse = reprojected.Diffuse;
        previousMoments = reprojected.Moments;
        previousLength = reprojected.Length;
        previousLength = isfinite(previousLength)
            ? clamp(previousLength, 0.0f, 128.0f)
            : 0.0f;
        previousHitDistance = reprojected.HitDistance;
    }

    float3 neighborhoodMinimum = noisy.rgb;
    float3 neighborhoodMaximum = noisy.rgb;
    float3 neighborhoodSum = noisy.rgb;
    float3 neighborhoodSquaredSum = noisy.rgb * noisy.rgb;
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
    // A stationary pixel does not advect history across its neighborhood, so
    // keep the wider sparse-path-tracing clamp there. Camera/object motion and
    // TAA jitter do reproject between pixels and require the tight clamp that
    // prevents visibility energy from walking across a shadow boundary.
    const float clampSigma = reprojectionPixels > 0.01f
        ? 0.5f
        : max(g_NeighborhoodClampSigma, 0.5f);
    neighborhoodMinimum = max(
        neighborhoodMinimum,
        neighborhoodMean - neighborhoodDeviation * clampSigma);
    neighborhoodMaximum = min(
        neighborhoodMaximum,
        neighborhoodMean + neighborhoodDeviation * clampSigma);
    // A 1-spp path tracer legitimately produces all-zero neighborhoods for
    // several frames on indirectly lit surfaces, so an empty observation must
    // not erase mature history. Once the current neighborhood contains actual
    // radiance, however, constrain every history age: otherwise repeated
    // subpixel jitter can transport a bright same-material estimate across a
    // shadow boundary without any geometric guide rejecting it.
    const bool bNeighborhoodHasRadiance = any(
        neighborhoodMaximum > float3(1e-5f, 1e-5f, 1e-5f));
    if (bValid && bHasObservation && bNeighborhoodHasRadiance)
    {
        previousDiffuse.rgb = clamp(
            previousDiffuse.rgb,
            neighborhoodMinimum,
            neighborhoodMaximum);
    }
    // Missing screen-space observations retain radiance but slowly age its
    // confidence. This prevents a temporarily covered SSGI sample from
    // remaining fully trusted forever after it leaves the observable domain.
    const float retainedHistoryLength = bHasObservation
        ? min(previousLength + observationWeight, 128.0f)
        : previousLength * 0.98f;
    const float historyLength = bValid
        ? retainedHistoryLength
        : observationWeight;
    const float blend = !bValid
        ? 1.0f
        : bHasObservation
            ? max(
                observationWeight / max(historyLength, observationWeight),
                g_MinimumBlend * observationWeight)
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
    const float centerDepth = g_HistoryDepthInput.Load(int3(pixel, 0));
    const float3 centerNormal = SafeNormal(
        g_HistoryNormalInput.Load(int3(pixel, 0)).xyz);
    const uint centerMaterial = g_HistoryMaterialInput.Load(int3(pixel, 0));
    const uint2 centerFullPixel = MapPixel(
        pixel,
        g_Dimensions,
        g_HistoryFixFullDimensions);
    const float3 centerAlbedo = DiffuseAlbedo(
        g_HistoryFixBaseColorMetalness.Load(int3(centerFullPixel, 0)));
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
                    int3(neighborFullPixel, 0)));
            const float roughness = g_HistoryFixNormalRoughness.Load(
                int3(neighborFullPixel, 0)).w;
            const float depthWeight = exp(
                -abs(depth - centerDepth) / max(g_DepthSigma, 1e-6f));
            const float normalWeight = pow(
                max(dot(centerNormal, normal), 0.0f),
                g_NormalPower);
            const float albedoWeight = exp(
                -length(albedo - centerAlbedo) / max(g_AlbedoSigma, 1e-6f));
            const float roughnessWeight = exp(
                -abs(roughness - centerRoughness) /
                max(g_RoughnessSigma, 1e-6f));
            const float weight = depthWeight * normalWeight *
                albedoWeight * roughnessWeight;
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

#if ATROUS
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
    float2 g_AtrousPadding;
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
Texture2D<float4> g_AtrousNormalRoughness : register(t7, space3);
Texture2D<float> g_AtrousConfidence : register(t8, space3);
Texture2D<float> g_AtrousHistoryLength : register(t9, space3);
RWTexture2D<float4> g_AtrousOutput : register(u0, space3);

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= g_Dimensions))
        return;
    const float4 center = g_AtrousInput.Load(int3(pixel, 0));
    const float centerDepth = g_AtrousDepth.Load(int3(pixel, 0));
    const float3 centerNormal = SafeNormal(
        g_AtrousNormal.Load(int3(pixel, 0)).xyz);
    const float centerHit = g_AtrousHitDistance.Load(int3(pixel, 0));
    const uint centerMaterial = g_AtrousMaterial.Load(int3(pixel, 0));
    const uint2 centerFullPixel = MapPixel(
        pixel,
        g_Dimensions,
        g_AtrousFullDimensions);
    const float3 centerAlbedo = DiffuseAlbedo(
        g_AtrousBaseColorMetalness.Load(int3(centerFullPixel, 0)));
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
    const float varianceOfMean = max(
        g_AtrousVariance.Load(int3(pixel, 0)),
        0.0f) / effectiveHistory;
    const float sigmaLuminance = max(
        sqrt(varianceOfMean) * g_PhiLuminance,
        max(Luminance(center.rgb) * 0.02f, 1e-4f));
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
            const float3 value = g_AtrousInput.Load(int3(neighbor, 0)).rgb;
            const float depth = g_AtrousDepth.Load(int3(neighbor, 0));
            const float3 normal = SafeNormal(
                g_AtrousNormal.Load(int3(neighbor, 0)).xyz);
            const float hit = g_AtrousHitDistance.Load(int3(neighbor, 0));
            const uint material = g_AtrousMaterial.Load(int3(neighbor, 0));
            if (material != centerMaterial || material == c_InvalidMaterial)
                continue;
            const uint2 neighborFullPixel = MapPixel(
                uint2(neighbor),
                g_Dimensions,
                g_AtrousFullDimensions);
            const float3 albedo = DiffuseAlbedo(
                g_AtrousBaseColorMetalness.Load(
                    int3(neighborFullPixel, 0)));
            const float roughness = g_AtrousNormalRoughness.Load(
                int3(neighborFullPixel, 0)).w;
            const float confidence = g_AtrousConfidence.Load(
                int3(neighbor, 0));
            const float historyLength = g_AtrousHistoryLength.Load(
                int3(neighbor, 0));
            const float spatialWeight = kernel[x + 2] * kernel[y + 2];
            const float depthWeight = exp(
                -abs(depth - centerDepth) /
                max(g_PhiDepth * max(abs(centerDepth), 1e-3f), 1e-6f));
            const float normalWeight = pow(
                max(dot(centerNormal, normal), 0.0f),
                g_PhiNormal);
            const float luminanceWeight = exp(
                -abs(Luminance(value) - Luminance(center.rgb)) /
                sigmaLuminance);
            // Zero hit distance means no observable secondary surface for
            // SSGI, not a geometric discontinuity. Apply the hit-distance
            // guide only when both samples own a real secondary hit so valid
            // neighboring illumination can fill low-coverage screen regions.
            const bool bCenterHasHit = centerHit > 1e-5f;
            const bool bNeighborHasHit = hit > 1e-5f;
            const float hitWeight = bCenterHasHit && bNeighborHasHit
                ? exp(
                    -abs(hit - centerHit) /
                    max(abs(centerHit) * 0.1f + 1e-3f, 1e-3f))
                : 1.0f;
            const float albedoWeight = exp(
                -length(albedo - centerAlbedo) / max(g_PhiAlbedo, 1e-6f));
            const float roughnessWeight = exp(
                -abs(roughness - centerRoughness) /
                max(g_PhiRoughness, 1e-6f));
            const float confidenceWeight = lerp(
                0.25f,
                1.0f,
                saturate(min(centerConfidence, confidence)));
            const float historyWeight = lerp(
                0.25f,
                1.0f,
                saturate(min(centerHistoryLength, historyLength) / 8.0f));
            const float weight = spatialWeight * depthWeight * normalWeight *
                luminanceWeight * hitWeight * albedoWeight * roughnessWeight *
                confidenceWeight * historyWeight;
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
    const float3 centerNormal = SafeNormal(
        g_FullNormal.Load(int3(pixel, 0)).xyz);
    const float4 centerBaseColorMetalness =
        g_UpsampleBaseColorMetalness.Load(int3(pixel, 0));
    const float3 centerAlbedo = DiffuseAlbedo(centerBaseColorMetalness);
    const float centerRoughness = g_FullNormal.Load(int3(pixel, 0)).w;
    const uint centerMaterial = g_UpsampleFullMaterial.Load(int3(pixel, 0));
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
                    int3(sampleFullPixel, 0)));
            const float roughness = g_FullNormal.Load(
                int3(sampleFullPixel, 0)).w;
            const float2 delta = abs(float2(samplePixel) - lowPosition);
            const float bilinearWeight =
                max(1.0f - delta.x, 0.0f) * max(1.0f - delta.y, 0.0f);
            const float depthWeight = exp(
                -abs(depth - centerDepth) / max(g_UpsampleDepthSigma, 1e-6f));
            const float normalWeight = pow(
                max(dot(centerNormal, normal), 0.0f),
                g_UpsampleNormalPower);
            const float albedoWeight = exp(
                -length(albedo - centerAlbedo) /
                max(g_UpsampleAlbedoSigma, 1e-6f));
            const float roughnessWeight = exp(
                -abs(roughness - centerRoughness) /
                max(g_UpsampleRoughnessSigma, 1e-6f));
            const float hitWeight = exp(
                -abs(hit - centerHit) /
                max(abs(centerHit) * 0.1f + 1e-3f, 1e-3f));
            const float weight = bilinearWeight * depthWeight * normalWeight *
                albedoWeight * roughnessWeight * hitWeight * confidence;
            sum += g_LowDiffuse.Load(int3(samplePixel, 0)).rgb * weight;
            hitSum += hit * weight;
            confidenceSum += confidence * weight;
            coverageSum += coverage * weight;
            weightSum += weight;
        }
    }
    const float3 fallback = g_LowDiffuse.Load(
        int3(fallbackPixel, 0)).rgb;
    const float fallbackHit = g_UpsampleLowHitDistance.Load(
        int3(fallbackPixel, 0));
    const float fallbackConfidence = g_UpsampleLowConfidence.Load(
        int3(fallbackPixel, 0));
    const float fallbackCoverage = g_UpsampleLowCoverage.Load(
        int3(fallbackPixel, 0));
    g_FullDiffuse[pixel] = float4(
        weightSum > 0.0f ? sum / weightSum : fallback,
        1.0f);
    g_FullHitDistance[pixel] = weightSum > 0.0f
        ? hitSum / weightSum
        : fallbackHit;
    g_FullConfidence[pixel] = weightSum > 0.0f
        ? saturate(confidenceSum / weightSum)
        : saturate(fallbackConfidence);
    g_FullCoverage[pixel] = weightSum > 0.0f
        ? saturate(coverageSum / weightSum)
        : saturate(fallbackCoverage);
}
#endif

#if REMODULATE
struct RemodulateConstants
{
    uint2 g_RemodulateDimensions;
    float g_RemodulateAlbedoEpsilon;
    float g_RemodulatePadding;
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
    const float3 emissive = max(
        g_RemodulateEmissive.Load(int3(pixel, 0)).rgb,
        0.0f);
    const float3 albedo = DiffuseAlbedo(baseColorMetalness);
    const float epsilon = max(
        g_RemodulateConstants.g_RemodulateAlbedoEpsilon,
        1e-6f);
    const bool bDiffuse = baseColorMetalness.w < 1.0f &&
        any(albedo > 0.0f);
    const bool bEmissive = any(emissive > epsilon);
    const float3 radiance = bDiffuse && !bEmissive
        ? max(g_FilteredIllumination.Load(int3(pixel, 0)).rgb, 0.0f) * albedo
        : 0.0f;
    g_RemodulatedDiffuse[pixel] = float4(
        all(isfinite(radiance)) ? radiance : 0.0f,
        bDiffuse && !bEmissive ? 1.0f : 0.0f);
}
#endif

#if !DEMODULATE && !TEMPORAL && !HISTORY_FIX && !VARIANCE && !ATROUS && !UPSAMPLE && !REMODULATE
[numthreads(1, 1, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x == 0xffffffffu)
        return;
}
#endif
