#ifndef HE_DIRECTIONAL_SHADOW_SAMPLING_HLSLI
#define HE_DIRECTIONAL_SHADOW_SAMPLING_HLSLI

float HE_DirectionalShadowDepthBias(float authoredBias, float depthScale, float referenceDepthScale)
{
    return authoredBias * depthScale / max(referenceDepthScale, 1e-6f);
}

float HE_DirectionalShadowFilterRadius(float radius, float texelScale, float referenceTexelScale)
{
    // Positive radii retain continuous hardware comparison. Exact zero keeps
    // the authored hard-shadow predicate instead of enabling PCF implicitly.
    return radius > 0.0f
        ? max(0.01f, (radius + 0.5f) * texelScale / max(referenceTexelScale, 1e-6f) - 0.5f)
        : 0.0f;
}

#endif
