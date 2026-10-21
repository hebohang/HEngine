#ifndef HE_SHADOW_CONTINUOUS_PCF_HLSLI
#define HE_SHADOW_CONTINUOUS_PCF_HLSLI

// Shared by opaque, deferred and custom-material Forward lighting.
// s0 remains the ordinary environment sampler. s1 is comparison-only.
SamplerComparisonState g_ShadowComparisonSampler : register(s1, space3);

float HE_ShadowBoxWeight(float texel, float center, float halfWidth)
{
    return max(0.0f, min(texel + 0.5f, center + halfWidth) -
        max(texel - 0.5f, center - halfWidth));
}

float HE_ContinuousShadowPcf(Texture2DArray<float> depthTexture,
    float2 uv, uint layer, uint2 size, float referenceDepth, float radius)
{
    // Radius zero also encodes per-light bUsePcf=false. Preserve its legacy
    // clamped point predicate; a global optimization must not enable PCF.
    if (radius <= 0.0f)
    {
        const int2 pixel = clamp(int2(uv * float2(size)), int2(0, 0),
            int2(size) - 1);
        return referenceDepth <= depthTexture.Load(int4(pixel, layer, 0))
            ? 1.0f : 0.0f;
    }
    // D32 contract: NVRHI comparison is LESS. One representable float below
    // the reference preserves the old reference <= stored-depth predicate.
    // Nonpositive references are fully lit for all normalized depth values.
    const float lessReference = referenceDepth > 0.0f
        ? asfloat(asuint(referenceDepth) - 1u) : -1e-30f;
    radius = clamp(radius, 0.0f, 4.0f);
    const float halfWidth = radius + 0.5f;
    const float2 center = uv * float2(size) - 0.5f;
    const int2 first = int2(floor(center - radius));
    const int2 last = int2(floor(center + radius)) + 1;
    float visibility = 0.0f;
    // Integrate a box of width 2*radius+1 texels. Pair adjacent weights into
    // hardware bilinear comparison: at most 4 taps for r=1, 25 for r=4.
    // Sample at the weighted texel center, not a rounded center/radius.
    [loop]
    for (int y = first.y; y <= last.y; y += 2)
    {
        const float wy0 = HE_ShadowBoxWeight(float(y), center.y, halfWidth);
        const float wy1 = HE_ShadowBoxWeight(float(y + 1), center.y, halfWidth);
        const float wy = wy0 + wy1;
        if (wy <= 0.0f) continue;
        [loop]
        for (int x = first.x; x <= last.x; x += 2)
        {
            const float wx0 = HE_ShadowBoxWeight(float(x), center.x, halfWidth);
            const float wx1 = HE_ShadowBoxWeight(float(x + 1), center.x, halfWidth);
            const float wx = wx0 + wx1;
            if (wx <= 0.0f) continue;
            const float2 sampleUv = (float2(x, y) + 0.5f +
                float2(wx1 / wx, wy1 / wy)) / float2(size);
            visibility += wx * wy * depthTexture.SampleCmpLevelZero(
                g_ShadowComparisonSampler, float3(sampleUv, float(layer)), lessReference);
        }
    }
    const float width = 2.0f * radius + 1.0f;
    return visibility / (width * width);
}
#endif
