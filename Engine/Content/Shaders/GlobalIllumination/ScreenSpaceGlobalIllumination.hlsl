#pragma htype cs CS

#include "Shared/BackendPushConstants.hlsli"

static const float c_Pi = 3.14159265358979323846f;
static const float c_TwoPi = 6.28318530717958647692f;
static const uint c_InvalidMaterial = 0xffffffffu;

cbuffer ScreenSpaceFrameConstants : register(b0, space0)
{
    float4x4 g_ViewProjection;
    float4x4 g_UnusedLightViewProjection;
    float4 g_CameraPosition;
    float4 g_UnusedDirectionalDirectionAndIntensity;
    float4 g_UnusedDirectionalColor;
    float4 g_UnusedPointPositionAndIntensity;
    float4 g_UnusedPointColorAndRadius;
    float4 g_UnusedShadowSettings;
    float4 g_UnusedIblSettings;
    float4x4 g_InverseViewProjection;
};

struct ScreenSpaceTraceConstants
{
    uint2 g_TraceDimensions;
    uint2 g_FullDimensions;
    uint g_FrameSequenceIndex;
    uint g_RayCount;
    uint g_StepCount;
    uint g_HiZStartMip;
    uint g_DepthMipCount;
    uint g_BinaryRefinementSteps;
    float g_MaximumRayDistance;
    float g_Thickness;
    float g_EdgeFade;
    float g_RayOriginOffset;
    uint g_LightCount;
    uint g_MaxBounces;
};
HE_DECLARE_PUSH_CONSTANTS(
    ScreenSpaceTraceConstants,
    g_TraceConstants,
    space3);

Texture2D<float> g_Depth : register(t0, space3);
Texture2D<float> g_DepthPyramid : register(t1, space3);
Texture2D<float4> g_NormalRoughness : register(t2, space3);
Texture2D<float4> g_BaseColorMetalness : register(t3, space3);
Texture2D<uint> g_MaterialId : register(t4, space3);
Texture2D<float4> g_OpaqueDirectHdr : register(t5, space3);

struct RayTracingEmissiveTriangle
{
    float4 Position0;
    float4 Position1;
    float4 Position2;
    float4 NormalArea;
    float4 RadianceCdf;
    uint4 Identity;
};

struct RayTracingGeometryRecord
{
    uint4 VertexIndexData;
    uint4 GeometryData;
};

StructuredBuffer<RayTracingEmissiveTriangle> g_EmissiveTriangles :
    register(t6, space3);
StructuredBuffer<RayTracingGeometryRecord> g_Geometries :
    register(t7, space3);
RWTexture2D<float4> g_RawDiffuse : register(u0, space3);
RWTexture2D<float> g_HitDistance : register(u1, space3);
RWTexture2D<float> g_Confidence : register(u2, space3);
RWTexture2D<float> g_Coverage : register(u3, space3);
RWTexture2D<float4> g_RawDirectDiffuse : register(u4, space3);
RWTexture2D<float4> g_RawIndirectDiffuse : register(u5, space3);

uint PcgHash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) *
        277803737u;
    return (word >> 22u) ^ word;
}

float R2Sample(uint sampleIndex, uint dimension, uint scramble)
{
    static const float c_R2First = 0.7548776662466927f;
    static const float c_R2Second = 0.5698402909980532f;
    const float slope = (dimension & 1u) == 0u
        ? c_R2First
        : c_R2Second;
    const uint hashed = PcgHash(scramble ^ (dimension * 0x9e3779b9u));
    const float rotation = (float(hashed) + 0.5f) *
        (1.0f / 4294967296.0f);
    return frac(0.5f + float(sampleIndex + 1u) * slope + rotation);
}

float3 SafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return all(isfinite(value)) && isfinite(lengthSquared) &&
        lengthSquared > 1e-20f
        ? value * rsqrt(lengthSquared)
        : fallback;
}

uint2 MapPixel(uint2 pixel, uint2 sourceDimensions, uint2 targetDimensions)
{
    return min(
        uint2((float2(pixel) + 0.5f) *
            float2(targetDimensions) / float2(sourceDimensions)),
        targetDimensions - 1u);
}

float3 ReconstructWorldPosition(float2 uv, float deviceDepth)
{
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 position = mul(
        g_InverseViewProjection,
        float4(ndc, deviceDepth, 1.0f));
    position.xyz /= max(abs(position.w), 1e-20f);
    return position.xyz;
}

bool ProjectWorldPosition(
    float3 worldPosition,
    out float2 uv,
    out float deviceDepth)
{
    const float4 clip = mul(g_ViewProjection, float4(worldPosition, 1.0f));
    if (!all(isfinite(clip)) || abs(clip.w) <= 1e-6f)
    {
        uv = 0.0f;
        deviceDepth = 1.0f;
        return false;
    }
    const float3 ndc = clip.xyz / abs(clip.w);
    uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
    deviceDepth = ndc.z;
    return all(uv >= 0.0f) && all(uv < 1.0f) &&
        deviceDepth >= 0.0f && deviceDepth < 1.0f;
}

float3 SampleCosineHemisphere(float3 normal, float2 randomValue)
{
    const float radius = sqrt(randomValue.x);
    const float angle = c_TwoPi * randomValue.y;
    float sineValue = 0.0f;
    float cosineValue = 1.0f;
    sincos(angle, sineValue, cosineValue);
    const float3 tangent = abs(normal.z) < 0.999f
        ? SafeNormalize(
            cross(float3(0.0f, 0.0f, 1.0f), normal),
            float3(1.0f, 0.0f, 0.0f))
        : SafeNormalize(
            cross(float3(0.0f, 1.0f, 0.0f), normal),
            float3(1.0f, 0.0f, 0.0f));
    const float3 bitangent = cross(normal, tangent);
    const float3 local = float3(
        radius * cosineValue,
        radius * sineValue,
        sqrt(max(1.0f - randomValue.x, 0.0f)));
    return SafeNormalize(
        tangent * local.x + bitangent * local.y + normal * local.z,
        normal);
}

bool IsBehindSurface(
    float3 rayPosition,
    float2 uv,
    out float surfaceLinearDepth,
    out float depthGap)
{
    const uint2 pixel = min(
        uint2(uv * float2(g_TraceConstants.g_FullDimensions)),
        g_TraceConstants.g_FullDimensions - 1u);
    const float sceneDepth = g_Depth.Load(int3(pixel, 0));
    if (!isfinite(sceneDepth) || sceneDepth >= 1.0f)
    {
        surfaceLinearDepth = 0.0f;
        depthGap = 0.0f;
        return false;
    }
    const float3 surfacePosition = ReconstructWorldPosition(uv, sceneDepth);
    float2 projectedUv;
    float rayDeviceDepth;
    if (!ProjectWorldPosition(rayPosition, projectedUv, rayDeviceDepth))
    {
        surfaceLinearDepth = 0.0f;
        depthGap = 0.0f;
        return false;
    }
    surfaceLinearDepth = length(surfacePosition - g_CameraPosition.xyz);
    depthGap = length(rayPosition - surfacePosition);
    return rayDeviceDepth >= sceneDepth;
}

bool TraceScreenSpaceRay(
    float3 origin,
    float3 direction,
    float traceMaximumDistance,
    out float2 hitUv,
    out float hitDistance,
    out float hitConfidence)
{
    hitUv = 0.0f;
    hitDistance = 0.0f;
    hitConfidence = 0.0f;
    const float maximumDistance = min(
        max(g_TraceConstants.g_MaximumRayDistance, 1e-3f),
        max(traceMaximumDistance, 1e-3f));
    const uint stepCount = max(g_TraceConstants.g_StepCount, 1u);
    const float baseStep = maximumDistance / float(stepCount);
    const uint maximumMip = max(
        g_TraceConstants.g_DepthMipCount,
        1u) - 1u;
    uint mip = min(g_TraceConstants.g_HiZStartMip, maximumMip);
    float previousDistance = 0.0f;
    float distanceAlongRay = baseStep;

    [loop]
    for (uint step = 0u; step < stepCount &&
        distanceAlongRay <= maximumDistance; ++step)
    {
        const float3 rayPosition = origin + direction * distanceAlongRay;
        float2 uv;
        float rayDeviceDepth;
        if (!ProjectWorldPosition(rayPosition, uv, rayDeviceDepth))
            return false;

        const uint2 mipDimensions = max(
            g_TraceConstants.g_FullDimensions >> mip,
            1u);
        const uint2 mipPixel = min(
            uint2(uv * float2(mipDimensions)),
            mipDimensions - 1u);
        const float hierarchyDepth = g_DepthPyramid.Load(
            int3(mipPixel, mip));
        if (!isfinite(hierarchyDepth) || hierarchyDepth >= 1.0f ||
            rayDeviceDepth < hierarchyDepth)
        {
            previousDistance = distanceAlongRay;
            distanceAlongRay += baseStep * float(1u << mip);
            mip = min(mip + 1u, maximumMip);
            continue;
        }
        if (mip > 0u)
        {
            --mip;
            continue;
        }

        float surfaceLinearDepth;
        float depthGap;
        if (!IsBehindSurface(
                rayPosition,
                uv,
                surfaceLinearDepth,
                depthGap))
        {
            previousDistance = distanceAlongRay;
            distanceAlongRay += baseStep;
            continue;
        }

        float lower = previousDistance;
        float upper = distanceAlongRay;
        [loop]
        for (uint refinement = 0u;
            refinement < g_TraceConstants.g_BinaryRefinementSteps;
            ++refinement)
        {
            const float middle = (lower + upper) * 0.5f;
            const float3 middlePosition = origin + direction * middle;
            float2 middleUv;
            float middleDeviceDepth;
            if (!ProjectWorldPosition(
                    middlePosition,
                    middleUv,
                    middleDeviceDepth))
            {
                upper = middle;
                continue;
            }
            float middleSurfaceDepth;
            float middleGap;
            if (IsBehindSurface(
                    middlePosition,
                    middleUv,
                    middleSurfaceDepth,
                    middleGap))
            {
                upper = middle;
                uv = middleUv;
                depthGap = middleGap;
            }
            else
            {
                lower = middle;
            }
        }
        const float3 refinedPosition = origin + direction * upper;
        float refinedDeviceDepth;
        if (!ProjectWorldPosition(refinedPosition, uv, refinedDeviceDepth) ||
            !IsBehindSurface(
                refinedPosition,
                uv,
                surfaceLinearDepth,
                depthGap) ||
            depthGap > max(g_TraceConstants.g_Thickness, 1e-4f))
        {
            return false;
        }
        const float edgeDistance = min(
            min(uv.x, 1.0f - uv.x),
            min(uv.y, 1.0f - uv.y));
        const float edgeConfidence = saturate(
            edgeDistance / max(g_TraceConstants.g_EdgeFade, 1e-5f));
        const float thicknessConfidence = 1.0f - saturate(
            depthGap / max(g_TraceConstants.g_Thickness, 1e-4f));
        hitUv = uv;
        hitDistance = upper;
        hitConfidence = edgeConfidence * thicknessConfidence;
        return hitConfidence > 0.0f;
    }
    return false;
}

bool IsEmissiveMaterial(uint materialId)
{
    if (materialId == c_InvalidMaterial)
        return false;
    [loop]
    for (uint lightIndex = 0u;
        lightIndex < g_TraceConstants.g_LightCount;
        ++lightIndex)
    {
        if (g_EmissiveTriangles[lightIndex].Identity.w == materialId)
            return true;
    }
    return false;
}

bool SampleEmissiveTriangle(
    uint sampleIndex,
    uint dimensionBase,
    uint scramble,
    float3 surfacePosition,
    out float3 lightPosition,
    out float3 lightDirection,
    out float lightDistance,
    out float3 emittedRadiance,
    out float solidAnglePdf)
{
    lightPosition = 0.0f;
    lightDirection = 0.0f;
    lightDistance = 0.0f;
    emittedRadiance = 0.0f;
    solidAnglePdf = 0.0f;
    const uint lightCount = g_TraceConstants.g_LightCount;
    if (lightCount == 0u)
        return false;

    const float select = R2Sample(
        sampleIndex,
        dimensionBase,
        scramble);
    uint selectedIndex = lightCount - 1u;
    float previousCdf = 0.0f;
    [loop]
    for (uint lightIndex = 0u; lightIndex < lightCount; ++lightIndex)
    {
        const float cdf = g_EmissiveTriangles[lightIndex].RadianceCdf.w;
        if (select <= cdf)
        {
            selectedIndex = lightIndex;
            break;
        }
        previousCdf = cdf;
    }

    const RayTracingEmissiveTriangle light =
        g_EmissiveTriangles[selectedIndex];
    const float selectionPdf = max(
        light.RadianceCdf.w - previousCdf,
        0.0f);
    const float root = sqrt(R2Sample(
        sampleIndex,
        dimensionBase + 1u,
        scramble));
    const float second = R2Sample(
        sampleIndex,
        dimensionBase + 2u,
        scramble);
    lightPosition =
        light.Position0.xyz * (1.0f - root) +
        light.Position1.xyz * (root * (1.0f - second)) +
        light.Position2.xyz * (root * second);
    const float3 offset = lightPosition - surfacePosition;
    const float distanceSquared = dot(offset, offset);
    if (!isfinite(distanceSquared) || distanceSquared <= 1e-16f ||
        selectionPdf <= 0.0f || light.NormalArea.w <= 0.0f)
    {
        return false;
    }

    lightDistance = sqrt(distanceSquared);
    lightDirection = offset / lightDistance;
    const float rawCosineAtLight = dot(
        SafeNormalize(light.NormalArea.xyz, -lightDirection),
        -lightDirection);
    const bool bTwoSided =
        g_Geometries[light.Identity.y].GeometryData.z != 0u;
    const float cosineAtLight = bTwoSided
        ? abs(rawCosineAtLight)
        : rawCosineAtLight;
    if (cosineAtLight <= 0.0f)
        return false;

    solidAnglePdf = selectionPdf * distanceSquared /
        max(light.NormalArea.w * cosineAtLight, 1e-20f);
    emittedRadiance = max(light.RadianceCdf.rgb, 0.0f);
    return solidAnglePdf > 0.0f &&
        all(isfinite(emittedRadiance));
}

bool IsScreenSpaceLightVisible(
    float3 surfacePosition,
    float3 surfaceNormal,
    float3 lightPosition,
    out float visibilityConfidence)
{
    visibilityConfidence = 0.0f;
    float2 lightUv;
    float lightDeviceDepth;
    if (!ProjectWorldPosition(lightPosition, lightUv, lightDeviceDepth))
        return false;

    const float3 rayOrigin = surfacePosition + surfaceNormal *
        g_TraceConstants.g_RayOriginOffset;
    const float3 offset = lightPosition - rayOrigin;
    const float distanceSquared = dot(offset, offset);
    if (!isfinite(distanceSquared) || distanceSquared <= 1e-16f)
        return false;
    const float targetDistance = sqrt(distanceSquared);
    if (targetDistance > g_TraceConstants.g_MaximumRayDistance)
        return false;

    const uint visibilitySteps = clamp(
        g_TraceConstants.g_StepCount / 2u,
        16u,
        64u);
    [loop]
    for (uint step = 1u; step < visibilitySteps; ++step)
    {
        // Exclude both endpoints so receiver and emitter do not self-occlude.
        const float segmentT = float(step) / float(visibilitySteps);
        const float3 rayPosition = lerp(rayOrigin, lightPosition, segmentT);
        float2 rayUv;
        float rayDeviceDepth;
        if (!ProjectWorldPosition(rayPosition, rayUv, rayDeviceDepth))
            return false;
        const uint2 depthPixel = min(
            uint2(rayUv * float2(g_TraceConstants.g_FullDimensions)),
            g_TraceConstants.g_FullDimensions - 1u);
        const float sceneDepth = g_DepthPyramid.Load(int3(depthPixel, 0));
        if (!isfinite(sceneDepth) || sceneDepth >= 1.0f ||
            rayDeviceDepth < sceneDepth)
        {
            continue;
        }
        // Device-depth deltas are projection dependent and made nearby box
        // faces self-shadow. Ignore a small endpoint guard, then classify a
        // blocker in world space using the same thickness contract as the
        // primary screen-space trace.
        const float endpointGuard = 2.0f / float(visibilitySteps);
        if (segmentT <= endpointGuard ||
            segmentT >= 1.0f - endpointGuard)
        {
            continue;
        }
        const float3 visibleSurface = ReconstructWorldPosition(
            rayUv,
            sceneDepth);
        const float worldGap = length(rayPosition - visibleSurface);
        // A foreground depth sample blocks the world-space light segment only
        // when the reconstructed point is actually close to that segment.
        // A large gap is unrelated projected foreground and must not produce
        // false shadowing.
        if (worldGap <= max(g_TraceConstants.g_Thickness, 1e-3f))
            return false;
    }

    const float edgeDistance = min(
        min(lightUv.x, 1.0f - lightUv.x),
        min(lightUv.y, 1.0f - lightUv.y));
    visibilityConfidence = saturate(
        edgeDistance / max(g_TraceConstants.g_EdgeFade, 1e-5f));
    return visibilityConfidence > 0.0f;
}

float3 EstimateEmissiveDirect(
    float3 surfacePosition,
    float3 surfaceNormal,
    float3 diffuseAlbedo,
    uint sampleIndex,
    uint dimensionBase,
    uint scramble)
{
    if (!any(diffuseAlbedo > 0.0f))
        return 0.0f;

    float3 lightPosition;
    float3 lightDirection;
    float lightDistance;
    float3 emittedRadiance;
    float lightPdf;
    if (!SampleEmissiveTriangle(
            sampleIndex,
            dimensionBase,
            scramble,
            surfacePosition,
            lightPosition,
            lightDirection,
            lightDistance,
            emittedRadiance,
            lightPdf))
    {
        return 0.0f;
    }

    const float cosineAtSurface = max(
        dot(surfaceNormal, lightDirection),
        0.0f);
    if (cosineAtSurface <= 0.0f)
        return 0.0f;

    float visibilityConfidence;
    if (!IsScreenSpaceLightVisible(
            surfacePosition,
            surfaceNormal,
            lightPosition,
            visibilityConfidence))
    {
        return 0.0f;
    }

    const float3 directRadiance =
        (diffuseAlbedo / c_Pi) * emittedRadiance *
        cosineAtSurface /
        max(lightPdf, 1e-20f);
    return all(isfinite(directRadiance))
        ? max(directRadiance * visibilityConfidence, 0.0f)
        : 0.0f;
}

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= g_TraceConstants.g_TraceDimensions))
        return;
    const uint2 fullPixel = MapPixel(
        pixel,
        g_TraceConstants.g_TraceDimensions,
        g_TraceConstants.g_FullDimensions);
    const float depth = g_Depth.Load(int3(fullPixel, 0));
    const uint materialId = g_MaterialId.Load(int3(fullPixel, 0));
    if (!isfinite(depth) || depth >= 1.0f)
    {
        g_RawDiffuse[pixel] = 0.0f;
        g_HitDistance[pixel] = 0.0f;
        g_Confidence[pixel] = 0.0f;
        g_Coverage[pixel] = 0.0f;
        g_RawDirectDiffuse[pixel] = 0.0f;
        g_RawIndirectDiffuse[pixel] = 0.0f;
        return;
    }

    const float2 uv = (float2(fullPixel) + 0.5f) /
        float2(g_TraceConstants.g_FullDimensions);
    const float3 worldPosition = ReconstructWorldPosition(uv, depth);
    const float3 normal = SafeNormalize(
        g_NormalRoughness.Load(int3(fullPixel, 0)).xyz,
        float3(0.0f, 1.0f, 0.0f));
    const float4 baseColorMetalness = g_BaseColorMetalness.Load(
        int3(fullPixel, 0));
    const float3 diffuseAlbedo = max(baseColorMetalness.rgb, 0.0f) *
        (1.0f - saturate(baseColorMetalness.w));
    if (materialId == c_InvalidMaterial ||
        IsEmissiveMaterial(materialId) ||
        !any(diffuseAlbedo > 0.0f))
    {
        // Emissive and fully metallic primaries do not own a diffuse GI
        // signal. Emission is sampled explicitly below from non-emissive
        // receivers instead of being albedo-demodulated as reflected light.
        g_RawDiffuse[pixel] = 0.0f;
        g_HitDistance[pixel] = 0.0f;
        g_Confidence[pixel] = 1.0f;
        g_Coverage[pixel] = 0.0f;
        g_RawDirectDiffuse[pixel] = 0.0f;
        g_RawIndirectDiffuse[pixel] = 0.0f;
        return;
    }
    const uint rayCount = clamp(g_TraceConstants.g_RayCount, 1u, 8u);
    const uint pixelScramble = PcgHash(
        pixel.x + pixel.y * g_TraceConstants.g_TraceDimensions.x);
    float3 radianceSum = 0.0f;
    float3 primaryDirectSum = 0.0f;
    float hitDistanceSum = 0.0f;
    float confidenceSum = 0.0f;
    uint hitCount = 0u;
    [loop]
    for (uint rayIndex = 0u; rayIndex < rayCount; ++rayIndex)
    {
        const uint sampleIndex =
            g_TraceConstants.g_FrameSequenceIndex * rayCount + rayIndex;
        const float2 randomValue = float2(
            R2Sample(sampleIndex, 0u, pixelScramble),
            R2Sample(sampleIndex, 1u, pixelScramble));
        primaryDirectSum += EstimateEmissiveDirect(
            worldPosition,
            normal,
            diffuseAlbedo,
            sampleIndex,
            2u,
            pixelScramble);
        float3 pathPosition = worldPosition;
        float3 pathNormal = normal;
        float3 throughput = diffuseAlbedo;
        [loop]
        for (uint bounce = 0u;
            bounce < clamp(g_TraceConstants.g_MaxBounces, 1u, 4u);
            ++bounce)
        {
            const uint dimensionBase = 8u + bounce * 5u;
            const float2 bounceRandom = bounce == 0u
                ? randomValue
                : float2(
                    R2Sample(sampleIndex, dimensionBase, pixelScramble),
                    R2Sample(sampleIndex, dimensionBase + 1u, pixelScramble));
            const float3 direction = SampleCosineHemisphere(
                pathNormal,
                bounceRandom);
            float2 hitUv;
            float rayHitDistance;
            float rayConfidence;
            if (!TraceScreenSpaceRay(
                    pathPosition + pathNormal *
                        g_TraceConstants.g_RayOriginOffset,
                    direction,
                    g_TraceConstants.g_MaximumRayDistance,
                    hitUv,
                    rayHitDistance,
                    rayConfidence))
            {
                break;
            }
            const uint2 hitPixel = min(
                uint2(hitUv * float2(g_TraceConstants.g_FullDimensions)),
                g_TraceConstants.g_FullDimensions - 1u);
            const float hitDepth = g_Depth.Load(int3(hitPixel, 0));
            const float3 hitPosition = ReconstructWorldPosition(hitUv, hitDepth);
            float3 hitNormal = SafeNormalize(
                g_NormalRoughness.Load(int3(hitPixel, 0)).xyz,
                -direction);
            if (dot(hitNormal, -direction) < 0.0f)
                hitNormal = -hitNormal;
            const float4 hitBaseColorMetalness = g_BaseColorMetalness.Load(
                int3(hitPixel, 0));
            const float3 hitDiffuseAlbedo = max(
                hitBaseColorMetalness.rgb,
                0.0f) * (1.0f - saturate(hitBaseColorMetalness.w));
            const uint hitMaterialId = g_MaterialId.Load(int3(hitPixel, 0));
            if (bounce == 0u)
            {
                hitDistanceSum += rayHitDistance;
                confidenceSum += rayConfidence;
                ++hitCount;
            }

            // OpaqueDirectHDR is captured before GI composition. Explicit NEE
            // owns emissive energy, so BSDF emitter hits contribute no second
            // copy of that path.
            const bool bEmissiveHit = IsEmissiveMaterial(hitMaterialId);
            const float3 opaqueDirect = bEmissiveHit
                ? 0.0f
                : max(g_OpaqueDirectHdr.Load(int3(hitPixel, 0)).rgb, 0.0f);
            const float3 emissiveDirect = bEmissiveHit
                ? 0.0f
                : EstimateEmissiveDirect(
                    hitPosition,
                    hitNormal,
                    hitDiffuseAlbedo,
                    sampleIndex,
                    dimensionBase + 2u,
                    pixelScramble);
            radianceSum += throughput * (opaqueDirect + emissiveDirect);
            if (bEmissiveHit || !any(hitDiffuseAlbedo > 0.0f))
                break;
            throughput *= hitDiffuseAlbedo;
            if (!any(throughput > 1e-5f))
                break;
            pathPosition = hitPosition;
            pathNormal = hitNormal;
        }
    }

    const float inverseRayCount = rcp(float(rayCount));
    const float coverage = float(hitCount) * inverseRayCount;
    const float confidence = confidenceSum * inverseRayCount;
    const float3 directDiffuseRadiance =
        primaryDirectSum * inverseRayCount;
    // A screen-space miss is often "no observable secondary surface", not a
    // physically black path. Average indirect radiance over the rays with a
    // valid screen-space observation and carry coverage separately so SVGF can
    // weight the estimate without baking missing-data bias into its value.
    const float coverageCompensation = hitCount > 0u
        ? min(float(rayCount) / float(hitCount), 2.0f)
        : 0.0f;
    const float3 indirectDiffuseRadiance =
        radianceSum * inverseRayCount * coverageCompensation;
    const float3 diffuseRadiance = directDiffuseRadiance +
        indirectDiffuseRadiance;
    g_RawDiffuse[pixel] = float4(
        all(isfinite(diffuseRadiance)) ? diffuseRadiance : 0.0f,
        coverage);
    g_RawDirectDiffuse[pixel] = float4(
        all(isfinite(directDiffuseRadiance))
            ? directDiffuseRadiance
            : 0.0f,
        1.0f);
    g_RawIndirectDiffuse[pixel] = float4(
        all(isfinite(indirectDiffuseRadiance))
            ? indirectDiffuseRadiance
            : 0.0f,
        coverage);
    g_HitDistance[pixel] = hitCount > 0u
        ? hitDistanceSum / float(hitCount)
        : 0.0f;
    g_Confidence[pixel] = saturate(coverage);
    g_Coverage[pixel] = saturate(coverage);
}
