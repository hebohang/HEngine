#pragma htype cs CS

#include "Shared/BackendPushConstants.hlsli"
#include "RasterSurfaceData.hlsli"

// Horizon visibility integral follows Jimenez et al., Practical Real-Time
// Strategies for Accurate Indirect Occlusion (ATVI-TR-19-01). This is an engine
// implementation of the published integral, not a vendored XeGTAO pipeline.
struct GtaoConstants
{
    float4x4 InverseViewProjection;
    float4 CameraPositionRadius;
    uint4 Dimensions; // native width/height, ceil half width/height
    float4 ViewForwardIntensity;
    // z is an exact integer packed as float to retain the 128-byte portable
    // push-constant ABI: operation bits0..1, slices bits2..5, steps bits6..10,
    // SSAO bit11, SSAO samples bits12..18, four-tap upsample bit19.
    float4 Parameters; // projected radius factor, normal bias, packed sampling, perspective flag
};
HE_DECLARE_PUSH_CONSTANTS(GtaoConstants, g_Gtao, space3);

Texture2D<float> g_Depth : register(t0, space3);
Texture2D<float4> g_Normal : register(t1, space3);
Texture2D<float> g_AoInput : register(t2, space3);
RWTexture2D<float> g_AoOutput : register(u0, space3);

static const float PI = 3.14159265359f;

// A half-resolution 8x8 workgroup needs a 10x10 guide tile. A native 8x8
// upsample workgroup covers four by four half pixels and needs only a 6x6 tile.
// Position, normal and nearest-covered selection are reused across the group;
// sampling positions, filter taps and all weights remain the reference formula.
groupshared float4 s_GuidePositionAo[100];
groupshared float3 s_GuideNormal[100];
groupshared uint2 s_GuideNativePixel[100];
groupshared uint s_GuideValid[100];

float3 Position(uint2 pixel, float depth)
{
    const float2 uv = (float2(pixel) + 0.5f) / float2(g_Gtao.Dimensions.xy);
    return HE_ReconstructRasterPosition(uv, depth, g_Gtao.InverseViewProjection);
}

uint2 RepresentativePixel(uint2 halfPixel)
{
    // The nearest covered native sample represents each 2x2 tile. The same
    // choice is reused by every bilateral stage, including odd-sized borders.
    const uint2 begin = halfPixel * 2u;
    const uint2 maximum = g_Gtao.Dimensions.xy - 1u;
    uint2 selected = min(begin, maximum);
    float nearest = g_Depth.Load(int3(selected, 0));
    [unroll]
    for (uint y = 0u; y < 2u; ++y)
    {
        [unroll]
        for (uint x = 0u; x < 2u; ++x)
        {
            const uint2 pixel = min(begin + uint2(x, y), maximum);
            const float depth = g_Depth.Load(int3(pixel, 0));
            if (depth < nearest) { nearest = depth; selected = pixel; }
        }
    }
    return selected;
}

float3 Normal(uint2 pixel)
{
    return HE_DecodeOctahedralNormal(g_Normal.Load(int3(pixel, 0)).xy);
}

float VisibilityArc(float horizon, float normalAngle)
{
    return 0.25f * (cos(normalAngle) + 2.0f * horizon * sin(normalAngle)
        - cos(2.0f * horizon - normalAngle));
}

float IntegrateHorizon(uint2 halfPixel, uint2 nativePixel, float depth)
{
    const float radius = g_Gtao.CameraPositionRadius.w;
    const float3 surfacePosition = Position(nativePixel, depth);
    float3 normal = Normal(nativePixel);
    const float3 viewDelta = g_Gtao.CameraPositionRadius.xyz - surfacePosition;
    const float3 view = g_Gtao.Parameters.w > 0.5f && dot(viewDelta, viewDelta) > 1e-12f
        ? normalize(viewDelta) : -g_Gtao.ViewForwardIntensity.xyz;
    // Strong shading normals must stay in the visible hemisphere.
    normal = normalize(normal + view * max(0.0001f - dot(normal, view), 0.0f));
    const float3 origin = surfacePosition + normal * g_Gtao.Parameters.y;
    const float viewDepth = max(dot(surfacePosition - g_Gtao.CameraPositionRadius.xyz,
        g_Gtao.ViewForwardIntensity.xyz), 0.0001f);
    const float projectedRadius = radius * g_Gtao.Parameters.x /
        (g_Gtao.Parameters.w > 0.5f ? viewDepth : 1.0f);
    // Nothing outside the image can be sampled; this bound is the image's
    // diagonal, rather than an arbitrary world-radius or quality reduction.
    const float pixelRadius = min(projectedRadius, length(float2(g_Gtao.Dimensions.xy)));
    if (pixelRadius <= 0.5f) return 1.0f;
    const float2 uv = (float2(nativePixel) + 0.5f) / float2(g_Gtao.Dimensions.xy);
    const uint slices = (uint(g_Gtao.Parameters.z) >> 2u) & 15u;
    const uint stepsPerSide = (uint(g_Gtao.Parameters.z) >> 6u) & 31u;
    const float rotation = float((halfPixel.x + 2u * halfPixel.y) & 3u) * (PI / (4.0f * float(slices)));
    float visibility = 0.0f;
    // Public sampling settings: default 4 slices x 8 steps x 2 sides = 64.
    // Every half-resolution pixel is updated; no temporal subsampling.
    [loop]
    for (uint slice = 0u; slice < slices; ++slice)
    {
        const float angle = rotation + float(slice) * (PI / float(slices));
        const float2 direction = float2(cos(angle), sin(angle));
        const float3 neighbor = HE_ReconstructRasterPosition(
            uv + direction / float2(g_Gtao.Dimensions.xy), depth, g_Gtao.InverseViewProjection);
        float3 tangent = neighbor - surfacePosition;
        tangent -= view * dot(tangent, view);
        tangent = normalize(tangent);
        const float3 slicePlaneNormal = normalize(cross(tangent, view));
        const float3 projectedNormal = normal - slicePlaneNormal * dot(normal, slicePlaneNormal);
        const float projectedLength = length(projectedNormal);
        const float normalAngle = atan2(dot(projectedNormal, tangent), dot(projectedNormal, view));
        const float baselinePositive = cos(normalAngle + PI * 0.5f);
        const float baselineNegative = cos(normalAngle - PI * 0.5f);
        float horizonPositive = baselinePositive;
        float horizonNegative = baselineNegative;
        [loop]
        for (uint step = 0u; step < stepsPerSide; ++step)
        {
            const float fraction = (float(step) + 0.5f) / float(stepsPerSide);
            const float distancePixels = max(1.0f, fraction * fraction * pixelRadius);
            [unroll]
            for (uint side = 0u; side < 2u; ++side)
            {
                const float sign = side == 0u ? 1.0f : -1.0f;
                const int2 samplePixel = int2(round(float2(nativePixel) + sign * direction * distancePixels));
                if (any(samplePixel < 0) || any(samplePixel >= int2(g_Gtao.Dimensions.xy))) continue;
                const float sampleDepth = g_Depth.Load(int3(samplePixel, 0));
                if (sampleDepth >= 0.999999f) continue;
                const float3 delta = Position(uint2(samplePixel), sampleDepth) - origin;
                const float distanceSquared = dot(delta, delta);
                if (distanceSquared < 1e-10f || distanceSquared >= radius * radius) continue;
                const float sampleDistance = sqrt(distanceSquared);
                const float horizonCos = dot(delta, view) / sampleDistance;
                const float falloff = saturate((radius - sampleDistance) / max(radius * 0.4f, 0.0001f));
                if (side == 0u)
                    horizonPositive = max(horizonPositive, lerp(baselinePositive, horizonCos, falloff));
                else
                    horizonNegative = max(horizonNegative, lerp(baselineNegative, horizonCos, falloff));
            }
        }
        float negativeAngle = -acos(clamp(horizonNegative, -1.0f, 1.0f));
        float positiveAngle = acos(clamp(horizonPositive, -1.0f, 1.0f));
        negativeAngle = normalAngle + clamp(negativeAngle - normalAngle, -PI * 0.5f, PI * 0.5f);
        positiveAngle = normalAngle + clamp(positiveAngle - normalAngle, -PI * 0.5f, PI * 0.5f);
        visibility += projectedLength * (VisibilityArc(negativeAngle, normalAngle)
            + VisibilityArc(positiveAngle, normalAngle));
    }
    return pow(saturate(visibility / float(slices)), g_Gtao.ViewForwardIntensity.w);
}

float IntegrateSsao(uint2 halfPixel, uint2 nativePixel, float depth)
{
    // Normal-weighted point-pair obscurance on a stratified screen-space disk.
    // Unlike GTAO this counts local obscurance samples, not horizon arcs.
    // The same world radius, visibility convention and edge filter serve both.
    const float3 position = Position(nativePixel, depth);
    const float3 normal = Normal(nativePixel);
    const float radius = g_Gtao.CameraPositionRadius.w;
    const float viewDepth = max(dot(position - g_Gtao.CameraPositionRadius.xyz,
        g_Gtao.ViewForwardIntensity.xyz), 0.0001f);
    const float projectedRadius = radius * g_Gtao.Parameters.x /
        (g_Gtao.Parameters.w > 0.5f ? viewDepth : 1.0f);
    const float pixelRadius = min(projectedRadius, length(float2(g_Gtao.Dimensions.xy)));
    if (pixelRadius <= 0.5f) return 1.0f;
    const uint sampleCount = (uint(g_Gtao.Parameters.z) >> 12u) & 127u;
    // Spatial rotation is stable with FXAA too; no unfiltered temporal noise.
    const float rotation = frac(52.9829189f * frac(dot(float2(halfPixel),
        float2(0.06711056f, 0.00583715f)))) * (2.0f * PI);
    float obscurance = 0.0f;
    [loop] for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
    {
        const float fraction = (float(sampleIndex) + 0.5f) / float(sampleCount);
        const float angle = rotation + float(sampleIndex) * 2.39996323f;
        const float2 offset = float2(cos(angle), sin(angle)) * max(1.0f, fraction * pixelRadius);
        const int2 samplePixel = int2(round(float2(nativePixel) + offset));
        if (any(samplePixel < 0) || any(samplePixel >= int2(g_Gtao.Dimensions.xy))) continue;
        const float sampleDepth = g_Depth.Load(int3(samplePixel, 0));
        if (sampleDepth >= 0.999999f) continue;
        const float3 delta = Position(uint2(samplePixel), sampleDepth) - position;
        const float distanceSquared = dot(delta, delta);
        if (distanceSquared < 1e-10f || distanceSquared >= radius * radius) continue;
        const float falloff = saturate(1.0f - distanceSquared / (radius * radius));
        obscurance += saturate((dot(normal, delta) - g_Gtao.Parameters.y) * rsqrt(distanceSquared)) * falloff;
    }
    // Cosine weighting: an unobstructed plane returns one. Radius limits the
    // near-field approximation; this does not see hidden/offscreen geometry.
    return pow(saturate(1.0f - 2.0f * obscurance / float(sampleCount)), g_Gtao.ViewForwardIntensity.w);
}

void LoadGuideTile(uint2 groupId, uint groupIndex, bool upsample)
{
    const uint span = upsample ? 6u : 10u;
    const int2 origin = int2(groupId * (upsample ? 4u : 8u)) - 1;
    for (uint index = groupIndex; index < span * span; index += 64u)
    {
        const int2 halfPixel = origin + int2(index % span, index / span);
        s_GuideValid[index] = 0u;
        s_GuidePositionAo[index] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        s_GuideNormal[index] = float3(0.0f, 0.0f, 1.0f);
        s_GuideNativePixel[index] = 0u;
        if (any(halfPixel < 0) || any(halfPixel >= int2(g_Gtao.Dimensions.zw))) continue;
        const uint2 representative = RepresentativePixel(uint2(halfPixel));
        const float depth = g_Depth.Load(int3(representative, 0));
        s_GuideNativePixel[index] = representative;
        if (depth >= 0.999999f) continue;
        s_GuidePositionAo[index] = float4(Position(representative, depth), g_AoInput.Load(int3(halfPixel, 0)));
        s_GuideNormal[index] = Normal(representative);
        s_GuideValid[index] = 1u;
    }
    GroupMemoryBarrierWithGroupSync();
}

float Bilateral(uint2 outputPixel, uint2 nativePixel, float3 centerPosition,
    float3 centerNormal, bool upsample, uint2 groupId)
{
    const int2 centerHalf = upsample ? int2(outputPixel / 2u) : int2(outputPixel);
    const bool fourTap = upsample && (uint(g_Gtao.Parameters.z) & (1u << 19u)) != 0u;
    // Native pixel centers map between half-resolution centers. Keep the four
    // enclosing samples; depth/normal rejection remains identical to nine-tap.
    const float2 halfCoordinate = (float2(outputPixel) + 0.5f) * 0.5f - 0.5f;
    const int2 filterBase = fourTap ? int2(floor(halfCoordinate)) : centerHalf - 1;
    const float2 blend = frac(halfCoordinate);
    const int filterWidth = fourTap ? 2 : 3;
    const uint span = upsample ? 6u : 10u;
    const int2 guideOrigin = int2(groupId * (upsample ? 4u : 8u)) - 1;
    const float radius = g_Gtao.CameraPositionRadius.w;
    const float centerViewDepth = abs(dot(centerPosition - g_Gtao.CameraPositionRadius.xyz,
        g_Gtao.ViewForwardIntensity.xyz));
    const float depthTolerance = max(radius * 0.1f, centerViewDepth * 0.002f);
    float weightedAo = 0.0f;
    float weightSum = 0.0f;
    [loop]
    for (int y = 0; y < filterWidth; ++y)
    {
        [loop]
        for (int x = 0; x < filterWidth; ++x)
        {
            const int2 halfPixel = filterBase + int2(x, y);
            if (any(halfPixel < 0) || any(halfPixel >= int2(g_Gtao.Dimensions.zw))) continue;
            const uint2 tilePixel = uint2(halfPixel - guideOrigin);
            const uint guideIndex = tilePixel.y * span + tilePixel.x;
            if (s_GuideValid[guideIndex] == 0u) continue;
            const uint2 representative = s_GuideNativePixel[guideIndex];
            const float3 delta = s_GuidePositionAo[guideIndex].xyz - centerPosition;
            const float planeDistance = abs(dot(delta, centerNormal));
            const float viewDistance = abs(dot(delta, g_Gtao.ViewForwardIntensity.xyz));
            const float normalAgreement = pow(saturate(dot(centerNormal, s_GuideNormal[guideIndex])), 32.0f);
            const float2 screenDistance = (float2(representative) - float2(nativePixel)) * 0.5f;
            const float spatialWeight = fourTap
                ? (x == 0 ? 1.0f - blend.x : blend.x) * (y == 0 ? 1.0f - blend.y : blend.y)
                : exp(-0.75f * dot(screenDistance, screenDistance));
            const float weight = normalAgreement * spatialWeight * exp(
                - planeDistance / max(radius * 0.05f, 0.0001f) - viewDistance / max(depthTolerance, 0.0001f));
            weightedAo += s_GuidePositionAo[guideIndex].w * weight;
            weightSum += weight;
        }
    }
    // No compatible guide means unoccluded, rather than borrowing a foreground
    // object's AO across a thin/background edge.
    return weightSum > 0.0001f ? saturate(weightedAo / weightSum) : 1.0f;
}

[numthreads(8, 8, 1)]
void CS(uint3 dispatchId : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    const uint operation = uint(g_Gtao.Parameters.z) & 3u;
    const bool upsample = operation == 2u;
    const uint2 dimensions = upsample ? g_Gtao.Dimensions.xy : g_Gtao.Dimensions.zw;
    const bool filter = operation != 0u;
    // Every group lane must participate in guide loading and its barrier,
    // including lanes beyond an odd-sized image's final partial workgroup.
    if (filter && g_Gtao.ViewForwardIntensity.w > 0.0f)
        LoadGuideTile(groupId.xy, groupIndex, upsample);
    if (any(dispatchId.xy >= dimensions)) return;
    const uint2 outputPixel = dispatchId.xy;
    if (g_Gtao.ViewForwardIntensity.w <= 0.0f)
    {
        g_AoOutput[outputPixel] = 1.0f;
        return;
    }
    uint2 nativePixel;
    float3 centerPosition = 0.0f;
    float3 centerNormal = 0.0f;
    float depth = 1.0f;
    if (filter && !upsample)
    {
        const uint2 tilePixel = outputPixel - groupId.xy * 8u + 1u;
        const uint guideIndex = tilePixel.y * 10u + tilePixel.x;
        if (s_GuideValid[guideIndex] == 0u) { g_AoOutput[outputPixel] = 1.0f; return; }
        nativePixel = s_GuideNativePixel[guideIndex];
        centerPosition = s_GuidePositionAo[guideIndex].xyz;
        centerNormal = s_GuideNormal[guideIndex];
    }
    else
    {
        nativePixel = upsample ? outputPixel : RepresentativePixel(outputPixel);
        depth = g_Depth.Load(int3(nativePixel, 0));
        if (depth >= 0.999999f) { g_AoOutput[outputPixel] = 1.0f; return; }
        if (filter) { centerPosition = Position(nativePixel, depth); centerNormal = Normal(nativePixel); }
    }
    float visibility;
    [branch] if (filter)
        visibility = Bilateral(outputPixel, nativePixel, centerPosition, centerNormal, upsample, groupId.xy);
    else if ((uint(g_Gtao.Parameters.z) & (1u << 11u)) != 0u)
        visibility = IntegrateSsao(outputPixel, nativePixel, depth);
    else
        visibility = IntegrateHorizon(outputPixel, nativePixel, depth);
    g_AoOutput[outputPixel] = isfinite(visibility) ? saturate(visibility) : 1.0f;
}
