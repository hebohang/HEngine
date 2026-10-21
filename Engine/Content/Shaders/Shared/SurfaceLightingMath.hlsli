#ifndef HE_SURFACE_LIGHTING_MATH_HLSLI
#define HE_SURFACE_LIGHTING_MATH_HLSLI

// Scalar-only functions are also compiled by RenderingShaderMathContract.
// Perceptual roughness is squared once to obtain the GGX slope parameter.
float HE_VisibilitySmithGGXCorrelated(float nDotV, float nDotL, float roughness)
{
    const float alpha = max(roughness * roughness, 0.0016f);
    const float alphaSquared = alpha * alpha;
    const float lambdaV = nDotL * sqrt(max(
        nDotV * nDotV * (1.0f - alphaSquared) + alphaSquared, 0.0f));
    const float lambdaL = nDotV * sqrt(max(
        nDotL * nDotL * (1.0f - alphaSquared) + alphaSquared, 0.0f));
    return 0.5f / max(lambdaV + lambdaL, 1e-6f);
}

// Mean of exp(-x*t), t in [0, 1]. The series avoids cancellation for
// horizontal rays and very small falloff; x is always nonnegative.
float HE_ExponentialSegmentMean(float x)
{
    return x < 0.01f
        ? 1.0f - x * (0.5f - x * (1.0f / 6.0f - x / 24.0f))
        : (1.0f - exp(-x)) / x;
}

float HE_HeightFogOpticalDepth(float cameraHeight, float surfaceHeight,
    float rayDistance, float startDistance, float baseHeight,
    float heightFalloff, float density)
{
    const float distance = max(rayDistance - max(startDistance, 0.0f), 0.0f);
    if (distance <= 0.0f || density <= 0.0f)
        return 0.0f;

    // Integrate only the segment after the authored fog start distance.
    const float beginHeight = surfaceHeight + (cameraHeight - surfaceHeight) *
        (distance / max(rayDistance, 1e-6f));
    const float low = min(beginHeight, surfaceHeight) - baseHeight;
    const float high = max(beginHeight, surfaceHeight) - baseHeight;
    const float falloff = max(heightFalloff, 0.0f);
    float meanDensity = 1.0f;
    if (high > 0.0f)
    {
        // Preserve HE's bounded density below BaseHeight. Split a crossing
        // segment there instead of approximating it with its midpoint.
        const float heightSpan = high - low;
        const float aboveSpan = high - max(low, 0.0f);
        const float aboveFraction = heightSpan > 1e-6f
            ? aboveSpan / heightSpan : 1.0f;
        const float aboveMean = exp(-falloff * max(low, 0.0f)) *
            HE_ExponentialSegmentMean(falloff * aboveSpan);
        meanDensity = (1.0f - aboveFraction) + aboveFraction * aboveMean;
    }
    return min(distance * density * meanDensity, 80.0f);
}

#endif
