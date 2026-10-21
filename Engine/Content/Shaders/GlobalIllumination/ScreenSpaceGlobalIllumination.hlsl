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
    float4 g_IblSettings;
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
    uint g_UseRasterSurface;
    uint g_UseMaterialAo;
    uint g_UseScreenAo;
    uint g_DiagnosticMode;
    uint g_CountDiagnostics;
    uint g_EnvironmentMisMode;
    uint2 g_CounterPadding;
    uint4 g_ReactivePixels;
    uint g_LocalReactiveBudget;
    uint g_ConfiguredRayCount;
    uint2 g_ReactivePadding;
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
Texture2D<float4> g_SurfaceEmissive : register(t8, space3);
TextureCube<float4> g_Environment : register(t9, space3);
Texture2D<float> g_EnvironmentCdf : register(t12, space3);
Texture2D<float4> g_PackedNormalRoughnessAo : register(t10, space3);
Texture2D<float> g_AmbientOcclusion : register(t11, space3);
SamplerState g_EnvironmentSampler : register(s0, space3);

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
RWStructuredBuffer<uint> g_DiagnosticCounters : register(u6, space3);

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

float3 SampleEnvironment(float3 direction)
{
    if (g_IblSettings.x <= 0.0f)
        return 0.0f;
    uint width, height, mipCount;
    g_Environment.GetDimensions(0, width, height, mipCount);
    // Scene-linear radiance, with the same intensity as raster IBL. Box mip 3
    // bounds high-frequency environment noise at 1-8 cosine samples/pixel;
    // this is an explicit angular prefilter, not the specular probe signal.
    const float mip = float(min(3u, max(mipCount, 1u) - 1u));
    const float3 radiance = g_Environment.SampleLevel(
        g_EnvironmentSampler, direction, mip).rgb * max(g_IblSettings.x, 0.0f);
    return all(isfinite(radiance)) ? max(radiance, 0.0f) : 0.0f;
}

// Proposal table represents 128x64 latitude-longitude cells; interval masses
// are evaluated from the exact uploaded float CDF used by inversion.
float EnvironmentProposalPdf(float3 direction)
{
    const float u = frac(atan2(direction.z, direction.x) / c_TwoPi + 0.5f);
    const float v = acos(clamp(direction.y, -1.0f, 1.0f)) / c_Pi;
    const uint x = min(uint(u * 128.0f), 127u);
    const uint y = min(uint(v * 64.0f), 63u);
    const uint cell = y * 128u + x;
    const float mass = g_EnvironmentCdf.Load(int3(cell + 1u, 0, 0)) -
        g_EnvironmentCdf.Load(int3(cell, 0, 0));
    const float solidAngle = (c_TwoPi / 128.0f) *
        (cos(c_Pi * float(y) / 64.0f) - cos(c_Pi * float(y + 1u) / 64.0f));
    return mass / solidAngle;
}

float3 SampleFoldedEnvironment(float3 normal, float2 randomValue, out float foldedPdf)
{
    const float u = min(randomValue.x, asfloat(0x3f7fffffu));
    uint low = 0u, high = 8192u;
    [loop] while (high - low > 1u)
    {
        const uint middle = (low + high) / 2u;
        if (g_EnvironmentCdf.Load(int3(middle, 0, 0)) <= u) low = middle;
        else high = middle;
    }
    const float begin = g_EnvironmentCdf.Load(int3(low, 0, 0));
    const float end = g_EnvironmentCdf.Load(int3(low + 1u, 0, 0));
    // Strict upper-bound search never selects a zero-width interval.
    const float within = min((u - begin) / (end - begin), asfloat(0x3f7fffffu));
    const uint x = low % 128u, y = low / 128u;
    const float phi = ((float(x) + within) / 128.0f - 0.5f) * c_TwoPi;
    const float cosine = lerp(cos(c_Pi * float(y) / 64.0f),
        cos(c_Pi * float(y + 1u) / 64.0f), randomValue.y);
    const float sine = sqrt(max(1.0f - cosine * cosine, 0.0f));
    const float3 direction = float3(sine * cos(phi), cosine, sine * sin(phi));
    // Retain the selected interval PDF: float(x)+within can round to a cell
    // boundary, so atan2/acos must not reassign this sampled preimage's mass.
    const float solidAngle = (c_TwoPi / 128.0f) *
        (cos(c_Pi * float(y) / 64.0f) - cos(c_Pi * float(y + 1u) / 64.0f));
    foldedPdf = (end - begin) / solidAngle + EnvironmentProposalPdf(-direction);
    return dot(normal, direction) >= 0.0f ? direction : -direction;
}

float3 DiffuseReflectance(float4 material, float roughness, float nDotV)
{
    const float metallic = saturate(material.w);
    const float3 f0 = lerp(0.04f, max(material.rgb, 0.0f), metallic);
    const float3 fresnel = f0 + (max(1.0f - roughness, f0) - f0) *
        pow(1.0f - saturate(nDotV), 5.0f);
    // Match the diffuse/specular energy partition used by raster IBL. With
    // cosine sampling, Lambert's 1/PI cancels the sampling PDF's 1/PI.
    return max(material.rgb, 0.0f) * (1.0f - metallic) * (1.0f - fresnel);
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
    if (!all(isfinite(clip)) || clip.w <= 1e-6f)
    {
        uv = 0.0f;
        deviceDepth = 1.0f;
        return false;
    }
    const float3 ndc = clip.xyz / clip.w;
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

float ClipRayDistance(float planeOrigin, float planeDirection, float distance)
{
    return planeDirection < 0.0f
        ? min(distance, max(planeOrigin, 0.0f) / -planeDirection)
        : distance;
}

bool TraceScreenSpaceRay(
    float3 origin,
    float3 direction,
    uint2 originPixel,
    float traceMaximumDistance,
    out float2 hitUv,
    out float hitDistance,
    out float hitConfidence,
    out float3 diagnostic)
{
    // First ray only: RGB = terminal code, backface rejections, reached meters.
    // 1=lit hit, 2=dark hit, 3=frustum exit, 4=max distance, 5=budget,
    // 6=thickness/discontinuity, 7=invalid projection, 8=invalid primary.
    // This is diagnostic data, never a transport or normalization weight.
    diagnostic = float3(7.0f, 0.0f, 0.0f);
    hitUv = 0.0f;
    hitDistance = 0.0f;
    hitConfidence = 0.0f;
    float maximumDistance = min(
        max(g_TraceConstants.g_MaximumRayDistance, 1e-3f),
        max(traceMaximumDistance, 1e-3f));
    const float4 clipOrigin = mul(g_ViewProjection, float4(origin, 1.0f));
    const float4 clipDirection = mul(g_ViewProjection, float4(direction, 0.0f));
    if (!all(isfinite(clipOrigin)) || !all(isfinite(clipDirection)) ||
        clipOrigin.w <= 1e-6f || clipOrigin.z < 0.0f ||
        clipOrigin.z >= clipOrigin.w ||
        any(abs(clipOrigin.xy) >= clipOrigin.w))
    {
        return false;
    }

    // Clip the world-space segment to the D3D clip volume before dividing by
    // w. In particular, a ray pointing toward the camera must stop at the
    // near plane; abs(w) incorrectly projects geometry behind the camera.
    const float requestedDistance = maximumDistance;
    maximumDistance = ClipRayDistance(clipOrigin.z, clipDirection.z, maximumDistance);
    maximumDistance = ClipRayDistance(clipOrigin.w - clipOrigin.z,
        clipDirection.w - clipDirection.z, maximumDistance);
    maximumDistance = ClipRayDistance(clipOrigin.w + clipOrigin.x,
        clipDirection.w + clipDirection.x, maximumDistance);
    maximumDistance = ClipRayDistance(clipOrigin.w - clipOrigin.x,
        clipDirection.w - clipDirection.x, maximumDistance);
    maximumDistance = ClipRayDistance(clipOrigin.w + clipOrigin.y,
        clipDirection.w + clipDirection.y, maximumDistance);
    maximumDistance = ClipRayDistance(clipOrigin.w - clipOrigin.y,
        clipDirection.w - clipDirection.y, maximumDistance);
    maximumDistance *= 0.99999f;
    if (maximumDistance <= 1e-5f)
        return false;

    const float4 clipEnd = clipOrigin + clipDirection * maximumDistance;
    const float2 inverseW = rcp(float2(clipOrigin.w, clipEnd.w));
    const float3 ndcOrigin = clipOrigin.xyz * inverseW.x;
    const float3 ndcEnd = clipEnd.xyz * inverseW.y;
    const float2 fullDimensions = float2(g_TraceConstants.g_FullDimensions);
    const float3 screenOrigin = float3(
        (ndcOrigin.xy * float2(0.5f, -0.5f) + 0.5f) * fullDimensions,
        ndcOrigin.z);
    const float3 screenEnd = float3(
        (ndcEnd.xy * float2(0.5f, -0.5f) + 0.5f) * fullDimensions,
        ndcEnd.z);
    const float3 screenDirection = screenEnd - screenOrigin;
    const float projectedLength = max(abs(screenDirection.x), abs(screenDirection.y));
    if (projectedLength < 1e-3f)
        return false;
    const float parameterEpsilon = min(1e-3f, 0.01f / projectedLength);
    const float2 inverseScreenDirection = float2(
        abs(screenDirection.x) > 1e-6f ? rcp(screenDirection.x) : 0.0f,
        abs(screenDirection.y) > 1e-6f ? rcp(screenDirection.y) : 0.0f);
    const uint maximumMip = max(
        g_TraceConstants.g_DepthMipCount,
        1u) - 1u;
    uint mip = min(g_TraceConstants.g_HiZStartMip, maximumMip);
    float parameter = 0.0f;
    const float endReason = maximumDistance < requestedDistance * 0.9999f ? 3.0f : 4.0f;

    [loop]
    for (uint step = 0u; step < g_TraceConstants.g_StepCount && parameter < 1.0f; ++step)
    {
        if (g_TraceConstants.g_DiagnosticMode != 0u)
            diagnostic.z = maximumDistance * parameter * inverseW.y /
                lerp(inverseW.x, inverseW.y, parameter);
        const float3 rayScreen = screenOrigin + screenDirection * parameter;
        const uint2 mipDimensions = max(
            g_TraceConstants.g_FullDimensions >> mip,
            1u);
        const uint2 mipPixel = min(
            uint2(max(rayScreen.xy, 0.0f)) >> mip,
            mipDimensions - 1u);
        const uint cellSize = 1u << mip;
        const float2 cellMinimum = float2(mipPixel * cellSize);
        // The last pyramid cell absorbs odd-sized source edges.
        const float2 cellMaximum = float2(uint2(
            mipPixel.x + 1u == mipDimensions.x
                ? g_TraceConstants.g_FullDimensions.x : (mipPixel.x + 1u) * cellSize,
            mipPixel.y + 1u == mipDimensions.y
                ? g_TraceConstants.g_FullDimensions.y : (mipPixel.y + 1u) * cellSize));
        const float2 boundary = float2(
            screenDirection.x >= 0.0f ? cellMaximum.x : cellMinimum.x,
            screenDirection.y >= 0.0f ? cellMaximum.y : cellMinimum.y);
        const float2 boundaryParameter = float2(
            abs(screenDirection.x) > 1e-6f
                ? (boundary.x - screenOrigin.x) * inverseScreenDirection.x : 2.0f,
            abs(screenDirection.y) > 1e-6f
                ? (boundary.y - screenOrigin.y) * inverseScreenDirection.y : 2.0f);
        const float cellExit = min(1.0f, max(parameter,
            min(boundaryParameter.x, boundaryParameter.y)));
        const float hierarchyDepth = g_DepthPyramid.Load(
            int3(mipPixel, mip));

        // Device depth is linear along the projected segment. A conservative
        // min-depth cell can only be skipped up to its actual screen boundary
        // or depth plane, never by an exponentially growing world-space step.
        float candidate = parameter;
        if (rayScreen.z < hierarchyDepth)
        {
            candidate = screenDirection.z > 1e-10f
                ? max(parameter, (hierarchyDepth - screenOrigin.z) / screenDirection.z)
                : 2.0f;
        }
        if (!isfinite(hierarchyDepth) || hierarchyDepth >= 1.0f || candidate >= cellExit)
        {
            parameter = cellExit + parameterEpsilon;
            mip = min(mip + 1u, maximumMip);
            continue;
        }
        if (mip > 0u)
        {
            parameter = candidate;
            --mip;
            continue;
        }

        if (all(mipPixel == originPixel))
        {
            parameter = cellExit + parameterEpsilon;
            continue;
        }

        if (g_TraceConstants.g_UseRasterSurface != 0u)
        {
            const float hitFacing = dot(
                g_NormalRoughness.Load(int3(mipPixel, 0)).xyz, -direction);
            if (!isfinite(hitFacing) || hitFacing <= 1e-3f)
            {
                if (g_TraceConstants.g_DiagnosticMode != 0u) diagnostic.y += 1.0f;
                // Pixel-constant depth can intersect a grazing ray leaving
                // a plane even though the ray never returns to that plane.
                // Its normal faces away from the incoming ray: skip this
                // pixel and keep searching instead of reflecting its own HDR.
                // Raster normals already orient double-sided visible faces;
                // the opposite side's radiance is not present in this buffer.
                parameter = cellExit + parameterEpsilon;
                // Stay at the leaf until a front/empty cell is found. Raising
                // the mip here repeatedly re-tests the same rejected plane.
                continue;
            }
        }

        if (screenDirection.z < -1e-10f)
        {
            // A camera-directed ray can enter this pixel behind its surface
            // and cross the depth plane before exiting. Test that intersection
            // instead of rejecting the entry point's larger thickness gap.
            // This is leaf-only: a coarse min-depth plane is merely a bound.
            const float planeParameter =
                (hierarchyDepth - screenOrigin.z) / screenDirection.z;
            if (planeParameter >= parameter && planeParameter < cellExit)
                candidate = planeParameter;
        }

        const float2 uv = (screenOrigin.xy + screenDirection.xy * candidate) / fullDimensions;
        const float distanceAlongRay = maximumDistance * candidate * inverseW.y /
            lerp(inverseW.x, inverseW.y, candidate);
        const float3 rayPosition = origin + direction * distanceAlongRay;
        const float3 surfacePosition = ReconstructWorldPosition(uv, hierarchyDepth);
        const float3 gap = rayPosition - surfacePosition;
        const float thickness = max(g_TraceConstants.g_Thickness, 1e-4f);
        if (dot(gap, gap) > thickness * thickness)
        {
            diagnostic.x = 6.0f;
            // A depth discontinuity is not a geometric intersection. Treat
            // the occluded region as unknown instead of leaking foreground light.
            return false;
        }
        const float edgeDistance = min(
            min(uv.x, 1.0f - uv.x),
            min(uv.y, 1.0f - uv.y));
        const float edgeConfidence = saturate(
            edgeDistance / max(g_TraceConstants.g_EdgeFade, 1e-5f));
        hitUv = uv;
        hitDistance = distanceAlongRay;
        // Thickness rejects false hits; it is not a radiometric attenuation.
        hitConfidence = edgeConfidence;
        diagnostic.x = hitConfidence > 0.0f ? 1.0f : 3.0f;
        diagnostic.z = distanceAlongRay;
        return hitConfidence > 0.0f;
    }
    diagnostic.x = parameter >= 1.0f ? endReason : 5.0f;
    if (parameter >= 1.0f) diagnostic.z = maximumDistance;
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

    if (g_TraceConstants.g_CountDiagnostics != 0u)
    {
        const uint visibilityRays = WaveActiveSum(1u);
        if (WaveIsFirstLane()) InterlockedAdd(g_DiagnosticCounters[5], visibilityRays);
    }
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
    const float4 normalRoughness = g_NormalRoughness.Load(int3(fullPixel, 0));
    const float3 normal = SafeNormalize(
        normalRoughness.xyz,
        float3(0.0f, 1.0f, 0.0f));
    const float4 baseColorMetalness = g_BaseColorMetalness.Load(
        int3(fullPixel, 0));
    const float roughness = saturate(normalRoughness.w);
    const float3 view = SafeNormalize(g_CameraPosition.xyz - worldPosition, normal);
    const bool bUnlit = g_TraceConstants.g_UseRasterSurface != 0u &&
        g_SurfaceEmissive.Load(int3(fullPixel, 0)).w > 0.5f;
    const float3 diffuseAlbedo = bUnlit ? 0.0f : DiffuseReflectance(
        baseColorMetalness, roughness, dot(normal, view));
    if (materialId == c_InvalidMaterial ||
        (g_TraceConstants.g_UseRasterSurface == 0u && IsEmissiveMaterial(materialId)) ||
        !any(diffuseAlbedo > 0.0f))
    {
        // Unlit and fully metallic primaries do not own diffuse GI. Lit
        // surfaces with emission still receive reflected indirect light.
        g_RawDiffuse[pixel] = 0.0f;
        g_HitDistance[pixel] = 0.0f;
        g_Confidence[pixel] = 1.0f;
        g_Coverage[pixel] = 0.0f;
        g_RawDirectDiffuse[pixel] = 0.0f;
        g_RawIndirectDiffuse[pixel] = 0.0f;
        return;
    }
    const bool localBudget = g_TraceConstants.g_LocalReactiveBudget != 0u;
    const bool reactivePixel = all(pixel >= g_TraceConstants.g_ReactivePixels.xy) &&
        all(pixel < g_TraceConstants.g_ReactivePixels.zw);
    const uint rayCount = clamp(localBudget && !reactivePixel
        ? g_TraceConstants.g_ConfiguredRayCount : g_TraceConstants.g_RayCount, 1u, 8u);
    const uint bounceCount = clamp(g_TraceConstants.g_MaxBounces, 1u, 4u);
    const float materialAo = g_TraceConstants.g_UseMaterialAo != 0u
        ? saturate(g_PackedNormalRoughnessAo.Load(int3(fullPixel, 0)).w) : 1.0f;
    float screenAo = 1.0f;
    if (g_TraceConstants.g_UseScreenAo != 0u)
    {
        uint aoWidth, aoHeight;
        g_AmbientOcclusion.GetDimensions(aoWidth, aoHeight);
        const uint2 aoPixel = MapPixel(fullPixel,
            g_TraceConstants.g_FullDimensions, uint2(aoWidth, aoHeight));
        screenAo = saturate(g_AmbientOcclusion.Load(int3(aoPixel, 0)));
    }
    const uint pixelScramble = PcgHash(
        pixel.x + pixel.y * g_TraceConstants.g_TraceDimensions.x);
    float3 radianceSum = 0.0f;
    float3 primaryDirectSum = 0.0f;
    float hitDistanceSum = 0.0f;
    uint hitCount = 0u;
    float3 primaryDiagnostic = float3(8.0f, 0.0f, 0.0f);
    float4 primaryHitLocation = 0.0f;
    uint4 diagnosticCounts = 0u;
    [loop]
    for (uint rayIndex = 0u; rayIndex < rayCount; ++rayIndex)
    {
        const uint sampleIndex =
            g_TraceConstants.g_FrameSequenceIndex * (localBudget ? 8u : rayCount) + rayIndex;
        const float2 randomValue = float2(
            R2Sample(sampleIndex, 0u, pixelScramble),
            R2Sample(sampleIndex, 1u, pixelScramble));
        // The legacy Cornell table has exact emitter material IDs. General
        // raster scenes sample emissive hits from the shared HDR surface;
        // mixing the restricted NEE table into that estimator double-counts
        // its subset while omitting ordinary textured/custom emitters.
        if (g_TraceConstants.g_UseRasterSurface == 0u)
        {
            primaryDirectSum += EstimateEmissiveDirect(
                worldPosition, normal, diffuseAlbedo, sampleIndex, 2u, pixelScramble);
        }
        float3 pathPosition = worldPosition;
        float3 pathNormal = normal;
        uint2 pathPixel = fullPixel;
        float3 throughput = diffuseAlbedo;
        [loop]
        for (uint bounce = 0u;
            bounce < bounceCount;
            ++bounce)
        {
            const uint dimensionBase = 8u + bounce * 5u;
            const float2 bounceRandom = bounce == 0u
                ? randomValue
                : float2(
                    R2Sample(sampleIndex, dimensionBase, pixelScramble),
                    R2Sample(sampleIndex, dimensionBase + 1u, pixelScramble));
            const bool environmentMis = g_TraceConstants.g_EnvironmentMisMode == 1u &&
                rayCount >= 2u && (rayCount & 1u) == 0u;
            // Equal cosine/environment counts at every even budget, including
            // the temporary eight-ray reactive window. Odd local pixel budgets
            // keep cosine sampling rather than use an unbalanced mixture.
            const bool bEnvironmentRay = (rayIndex & 1u) != 0u;
            float3 direction = SampleCosineHemisphere(pathNormal, bounceRandom);
            float sampledFoldedPdf = 0.0f;
            [branch] if (environmentMis && bEnvironmentRay)
                direction = SampleFoldedEnvironment(pathNormal, bounceRandom, sampledFoldedPdf);
            [branch] if (environmentMis)
            {
                const float cosinePdf = max(dot(pathNormal, direction), 0.0f) / c_Pi;
                const float foldedPdf = bEnvironmentRay ? sampledFoldedPdf :
                    EnvironmentProposalPdf(direction) + EnvironmentProposalPdf(-direction);
                const float mixturePdf = 0.5f * (cosinePdf + foldedPdf);
                // Weight complete incoming before hit/miss/confidence branches.
                // MaxBounces==1 is enforced by the CPU; do not multiply later bounces.
                throughput *= mixturePdf > 0.0f ? cosinePdf / mixturePdf : 0.0f;
            }
            float2 hitUv;
            float rayHitDistance;
            float rayConfidence;
            float3 rayDiagnostic;
            diagnosticCounts.x += bounce == 0u ? 1u : 0u;
            ++diagnosticCounts.y;
            const bool bScreenHit = TraceScreenSpaceRay(
                    pathPosition + pathNormal *
                        g_TraceConstants.g_RayOriginOffset,
                    direction,
                    pathPixel,
                    g_TraceConstants.g_MaximumRayDistance,
                    hitUv,
                    rayHitDistance,
                    rayConfidence, rayDiagnostic);
            diagnosticCounts.z += bScreenHit ? 1u : 0u;
            diagnosticCounts.w += bScreenHit ? 0u : 1u;
            if (rayIndex == 0u && bounce == 0u) primaryDiagnostic = rayDiagnostic;
            if (!bScreenHit)
            {
                // Screen-space misses have unknown geometry. The active
                // environment supplies the missing radiance instead of black
                // or a boosted neighboring hit. GTAO attenuates this fallback
                // only; observed screen-space transport already has visibility.
                radianceSum += throughput * SampleEnvironment(direction) * screenAo;
                break;
            }
            const uint2 hitPixel = min(
                uint2(hitUv * float2(g_TraceConstants.g_FullDimensions)),
                g_TraceConstants.g_FullDimensions - 1u);
            if (g_TraceConstants.g_DiagnosticMode != 0u && rayIndex == 0u && bounce == 0u)
            {
                primaryDiagnostic.x = any(g_OpaqueDirectHdr.Load(int3(hitPixel, 0)).rgb > 1e-5f) ? 1.0f : 2.0f;
                primaryHitLocation = float4(hitUv,
                    dot(g_OpaqueDirectHdr.Load(int3(hitPixel, 0)).rgb, float3(.2126f, .7152f, .0722f)), 1.0f);
            }
            if (bounce == 0u)
            {
                hitDistanceSum += rayHitDistance;
                ++hitCount;
            }

            // OpaqueDirectHDR is captured before GI composition. Legacy NEE
            // owns its table's emitters; complete raster surfaces instead
            // include visible emission in their screen-space hit estimator.
            const bool bRasterSurface = g_TraceConstants.g_UseRasterSurface != 0u;
            const bool bLegacyEmissiveHit = !bRasterSurface &&
                IsEmissiveMaterial(g_MaterialId.Load(int3(hitPixel, 0)));
            const float3 opaqueDirect = bLegacyEmissiveHit
                ? 0.0f
                : max(g_OpaqueDirectHdr.Load(int3(hitPixel, 0)).rgb, 0.0f);
            if (bRasterSurface)
            {
                float3 incoming = opaqueDirect;
                if (rayConfidence < 1.0f)
                    incoming = lerp(SampleEnvironment(direction) * screenAo,
                        incoming, rayConfidence);
                radianceSum += throughput * incoming;
                // A hit on the pre-GI HDR already supplies the first bounce.
                // Only a continued path needs the hit's material and position.
                if (bounce + 1u == bounceCount)
                    break;
            }

            const float hitDepth = g_Depth.Load(int3(hitPixel, 0));
            const float3 hitPosition = ReconstructWorldPosition(hitUv, hitDepth);
            const float4 hitNormalRoughness = g_NormalRoughness.Load(int3(hitPixel, 0));
            float3 hitNormal = SafeNormalize(hitNormalRoughness.xyz, -direction);
            if (dot(hitNormal, -direction) < 0.0f)
                hitNormal = -hitNormal;
            const float4 hitBaseColorMetalness = g_BaseColorMetalness.Load(int3(hitPixel, 0));
            const float4 surfaceEmission = g_SurfaceEmissive.Load(int3(hitPixel, 0));
            const bool bHitUnlit = bRasterSurface && surfaceEmission.w > 0.5f;
            const float3 hitDiffuseAlbedo = bHitUnlit ? 0.0f : DiffuseReflectance(
                hitBaseColorMetalness, saturate(hitNormalRoughness.w), dot(hitNormal, -direction));
            const bool bEmissiveHit = bRasterSurface
                ? any(surfaceEmission.rgb > 0.0f) || surfaceEmission.w > 0.5f
                : bLegacyEmissiveHit;
            if (!bRasterSurface)
            {
                const float3 emissiveDirect = bEmissiveHit ? 0.0f : EstimateEmissiveDirect(
                    hitPosition, hitNormal, hitDiffuseAlbedo,
                    sampleIndex, dimensionBase + 2u, pixelScramble);
                float3 incoming = opaqueDirect + emissiveDirect;
                if (rayConfidence < 1.0f)
                    incoming = lerp(SampleEnvironment(direction) * screenAo,
                        incoming, rayConfidence);
                radianceSum += throughput * incoming;
            }
            if (bounce + 1u == bounceCount || bEmissiveHit || !any(hitDiffuseAlbedo > 0.0f))
                break;
            throughput *= hitDiffuseAlbedo * saturate(rayConfidence);
            if (!any(throughput > 1e-5f))
                break;
            pathPosition = hitPosition;
            pathNormal = hitNormal;
            pathPixel = hitPixel;
        }
    }

    if (g_TraceConstants.g_CountDiagnostics != 0u)
    {
        const uint4 waveCounts = uint4(WaveActiveSum(diagnosticCounts.x), WaveActiveSum(diagnosticCounts.y),
            WaveActiveSum(diagnosticCounts.z), WaveActiveSum(diagnosticCounts.w));
        const uint wavePixels = WaveActiveSum(1u);
        if (WaveIsFirstLane())
        {
            InterlockedAdd(g_DiagnosticCounters[0], waveCounts.x);
            InterlockedAdd(g_DiagnosticCounters[1], waveCounts.y);
            InterlockedAdd(g_DiagnosticCounters[2], waveCounts.z);
            InterlockedAdd(g_DiagnosticCounters[3], waveCounts.w);
            InterlockedAdd(g_DiagnosticCounters[4], wavePixels);
            // c6 enabled pixels; c7 requested-but-fallback pixels. Existing c0..5 unchanged.
            if (g_TraceConstants.g_EnvironmentMisMode == 1u)
                InterlockedAdd(g_DiagnosticCounters[6], wavePixels);
            else if (g_TraceConstants.g_EnvironmentMisMode != 0u)
                InterlockedAdd(g_DiagnosticCounters[7], wavePixels);
        }
    }
    const float inverseRayCount = rcp(float(rayCount));
    // With MIS enabled this is proposal-sample hit fraction, not cosine coverage.
    // HitDistance is likewise a proposal-sample guide; no physical-unbiasedness claim
    // is made for the nonlinear denoiser consuming these unchanged guide definitions.
    const float coverage = float(hitCount) * inverseRayCount;
    const float3 directDiffuseRadiance =
        primaryDirectSum * inverseRayCount;
    // Every ray now has an observation (screen hit or real environment).
    // Average all rays once: valid-hit compensation would double count light.
    // Material AO modulates reflected indirect radiance, not source emission
    // or the separately owned analytic direct-light signal.
    const float3 indirectDiffuseRadiance =
        radianceSum * inverseRayCount * materialAo;
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
    // Raw alpha retains screen-hit coverage for diagnosis. Denoiser coverage
    // means observed transport, including environment, so all-miss surfaces
    // update history instead of retaining stale screen-space illumination.
    g_Confidence[pixel] = 1.0f;
    g_Coverage[pixel] = 1.0f;
    if (g_TraceConstants.g_DiagnosticMode == 1u)
        g_RawDirectDiffuse[pixel] = float4(primaryDiagnostic, 1.0f);
    else if (g_TraceConstants.g_DiagnosticMode == 2u)
        g_RawDirectDiffuse[pixel] = primaryHitLocation;
}
