#define HE_TEXTURE_SAMPLING_NO_DERIVATIVES
#include "Shared/TextureSampling.hlsli"
#pragma htype lib
#pragma hprofile lib lib_6_5

#include "Shared/BackendPushConstants.hlsli"

static const float c_Pi = 3.14159265358979323846f;
static const float c_TwoPi = 6.28318530717958647692f;
static const uint c_InvalidIndex = 0xffffffffu;

struct RayTracingVertex
{
    float4 Position;
    float4 Normal;
    float4 TexCoord;
    float4 Tangent;
    float4 Bitangent;
    float4 Color;
};

struct RayTracingGeometryRecord
{
    uint4 VertexIndexData;
    uint4 GeometryData;
};

struct RayTracingMaterialRecord
{
    float4 BaseColorMetallic;
    float4 EmissiveRoughness;
    uint4 Textures0;
    uint4 Textures1;
    float4 Surface;
    HETextureSampling Sampling[7];
};

struct RayTracingInstanceRecord
{
    float4x4 World;
    float4x4 WorldInverseTranspose;
    float4x4 PreviousWorld;
    uint4 Indices;
};

struct RayTracingEmissiveTriangle
{
    float4 Position0;
    float4 Position1;
    float4 Position2;
    float4 NormalArea;
    float4 RadianceCdf;
    uint4 Identity;
};

struct HitPayload
{
    float3 Position;
    float RayT;
    float3 GeometricNormal;
    uint MaterialIndex;
    float3 ShadingNormal;
    uint InstanceIndex;
    uint GeometryIndex;
    uint PrimitiveIndex;
    uint Hit;
    uint FrontFace;
};

struct RealtimeRtgiConstants
{
    float4x4 g_InverseViewProjection;
    float4 g_CameraPositionAndMaxDistance;
    // xy: trace dimensions, z: frame sequence index, w: maximum diffuse bounces.
    uint4 g_DimensionsFrameBounces;
    // x: fixed seed, y: emissive triangle count, zw: reserved.
    uint4 g_SamplingSettings;
    float4 g_EnvironmentRadiance;
};
HE_DECLARE_PUSH_CONSTANTS(
    RealtimeRtgiConstants,
    g_RealtimeRtgiConstants,
    space0);

#define g_InverseViewProjection g_RealtimeRtgiConstants.g_InverseViewProjection
#define g_CameraPositionAndMaxDistance \
    g_RealtimeRtgiConstants.g_CameraPositionAndMaxDistance
#define g_DimensionsFrameBounces \
    g_RealtimeRtgiConstants.g_DimensionsFrameBounces
#define g_SamplingSettings g_RealtimeRtgiConstants.g_SamplingSettings
#define g_EnvironmentRadiance g_RealtimeRtgiConstants.g_EnvironmentRadiance

RaytracingAccelerationStructure g_SceneTlas : register(t0, space2);
StructuredBuffer<RayTracingVertex> g_Vertices : register(t1, space2);
ByteAddressBuffer g_Indices : register(t2, space2);
StructuredBuffer<RayTracingGeometryRecord> g_Geometries : register(t3, space2);
StructuredBuffer<RayTracingMaterialRecord> g_Materials : register(t4, space2);
StructuredBuffer<RayTracingInstanceRecord> g_Instances : register(t5, space2);
StructuredBuffer<RayTracingEmissiveTriangle> g_EmissiveTriangles : register(t6, space2);

Texture2D<float> g_SurfaceDepth : register(t0, space3);
Texture2D<float4> g_SurfaceNormalRoughness : register(t1, space3);
Texture2D<float4> g_SurfaceBaseColorMetalness : register(t2, space3);
Texture2D<float4> g_SurfaceEmissive : register(t3, space3);
Texture2D<float2> g_SurfaceMotionVectors : register(t4, space3);
Texture2D<uint> g_SurfaceMaterialId : register(t5, space3);
RWTexture2D<float4> g_NoisyDiffuse : register(u0, space3);
RWTexture2D<float4> g_RawDirectDiffuse : register(u1, space3);
RWTexture2D<float4> g_RawIndirectDiffuse : register(u2, space3);
RWTexture2D<float> g_HitDistance : register(u3, space3);
RWTexture2D<float> g_Confidence : register(u4, space3);

uint PcgHash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

struct RandomSequence
{
    uint SampleIndex;
    uint Dimension;
    uint Scramble;
};

float RandomFloat(inout RandomSequence sequence)
{
    // Generalized golden-ratio additive recurrence. Every path dimension is
    // independently Cranley-Patterson rotated per pixel, while consecutive
    // frames remain a deterministic low-discrepancy sequence.
    static const float c_R2First = 0.7548776662466927f;
    static const float c_R2Second = 0.5698402909980532f;
    const uint dimension = sequence.Dimension++;
    const uint dimensionScramble = PcgHash(
        sequence.Scramble ^ (dimension * 0x9e3779b9u));
    const float rotation = (float(dimensionScramble) + 0.5f) *
        (1.0f / 4294967296.0f);
    const float slope = (dimension & 1u) == 0u
        ? c_R2First
        : c_R2Second;
    return frac(
        0.5f + float(sequence.SampleIndex + 1u) * slope + rotation);
}

float3 SafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return all(isfinite(value)) && isfinite(lengthSquared) && lengthSquared > 1e-20f
        ? value * rsqrt(lengthSquared)
        : fallback;
}

float RayOffset(float3 position)
{
    const float scale = max(abs(position.x), max(abs(position.y), abs(position.z)));
    return max(1e-5f, 2e-5f * max(scale, 1.0f));
}

float PowerHeuristic(float firstPdf, float secondPdf)
{
    const float firstSquared = firstPdf * firstPdf;
    const float secondSquared = secondPdf * secondPdf;
    return firstSquared / max(firstSquared + secondSquared, 1e-20f);
}

float3 SampleCosineHemisphere(float3 normal, float2 randomValue, out float pdf)
{
    const float radius = sqrt(randomValue.x);
    const float angle = c_TwoPi * randomValue.y;
    float sineValue = 0.0f;
    float cosineValue = 1.0f;
    sincos(angle, sineValue, cosineValue);
    const float3 tangent = abs(normal.z) < 0.999f
        ? SafeNormalize(cross(float3(0.0f, 0.0f, 1.0f), normal), float3(1.0f, 0.0f, 0.0f))
        : SafeNormalize(cross(float3(0.0f, 1.0f, 0.0f), normal), float3(1.0f, 0.0f, 0.0f));
    const float3 bitangent = cross(normal, tangent);
    const float3 local = float3(
        radius * cosineValue,
        radius * sineValue,
        sqrt(max(1.0f - randomValue.x, 0.0f)));
    const float3 direction = SafeNormalize(
        tangent * local.x + bitangent * local.y + normal * local.z,
        normal);
    pdf = max(dot(normal, direction), 0.0f) / c_Pi;
    return direction;
}

HitPayload TraceScene(float3 origin, float3 direction, float maximumDistance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0f;
    ray.TMax = maximumDistance;
    HitPayload payload = (HitPayload)0;
    payload.MaterialIndex = c_InvalidIndex;
    payload.InstanceIndex = c_InvalidIndex;
    payload.GeometryIndex = c_InvalidIndex;
    payload.PrimitiveIndex = c_InvalidIndex;
    TraceRay(
        g_SceneTlas,
        RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
        0xff,
        0,
        1,
        0,
        ray,
        payload);
    return payload;
}

bool SampleLight(
    inout RandomSequence randomSequence,
    float3 surfacePosition,
    out float3 direction,
    out float distanceToLight,
    out float3 radiance,
    out float solidAnglePdf)
{
    direction = 0.0f;
    distanceToLight = 0.0f;
    radiance = 0.0f;
    solidAnglePdf = 0.0f;
    const uint lightCount = g_SamplingSettings.y;
    if (lightCount == 0u)
        return false;
    const float select = RandomFloat(randomSequence);
    uint selectedIndex = lightCount - 1u;
    float previousCdf = 0.0f;
    [loop]
    for (uint index = 0u; index < lightCount; ++index)
    {
        const float cdf = g_EmissiveTriangles[index].RadianceCdf.w;
        if (select <= cdf)
        {
            selectedIndex = index;
            break;
        }
        previousCdf = cdf;
    }
    const RayTracingEmissiveTriangle light = g_EmissiveTriangles[selectedIndex];
    const float selectionPdf = max(light.RadianceCdf.w - previousCdf, 0.0f);
    const float root = sqrt(RandomFloat(randomSequence));
    const float second = RandomFloat(randomSequence);
    const float3 lightPosition =
        light.Position0.xyz * (1.0f - root) +
        light.Position1.xyz * (root * (1.0f - second)) +
        light.Position2.xyz * (root * second);
    const float3 offset = lightPosition - surfacePosition;
    const float distanceSquared = dot(offset, offset);
    if (distanceSquared <= 1e-16f || !isfinite(distanceSquared))
        return false;
    distanceToLight = sqrt(distanceSquared);
    direction = offset / distanceToLight;
    const bool twoSided =
        g_Geometries[light.Identity.y].GeometryData.z != 0u;
    const float rawCosineAtLight = dot(light.NormalArea.xyz, -direction);
    const float cosineAtLight = twoSided
        ? abs(rawCosineAtLight)
        : rawCosineAtLight;
    if (cosineAtLight <= 0.0f || selectionPdf <= 0.0f || light.NormalArea.w <= 0.0f)
        return false;
    solidAnglePdf = selectionPdf * distanceSquared /
        max(light.NormalArea.w * cosineAtLight, 1e-20f);
    radiance = light.RadianceCdf.rgb;
    return solidAnglePdf > 0.0f;
}

float EmissiveHitSolidAnglePdf(
    HitPayload hit,
    float3 previousPosition,
    float3 incomingDirection)
{
    const uint lightCount = g_SamplingSettings.y;
    float previousCdf = 0.0f;
    [loop]
    for (uint lightIndex = 0u; lightIndex < lightCount; ++lightIndex)
    {
        const RayTracingEmissiveTriangle light =
            g_EmissiveTriangles[lightIndex];
        const float selectionPdf = max(
            light.RadianceCdf.w - previousCdf,
            0.0f);
        previousCdf = light.RadianceCdf.w;
        if (light.Identity.x != hit.InstanceIndex ||
            light.Identity.y != hit.GeometryIndex ||
            light.Identity.z != hit.PrimitiveIndex)
        {
            continue;
        }
        const float distanceSquared = dot(
            hit.Position - previousPosition,
            hit.Position - previousPosition);
        const bool twoSided =
            g_Geometries[light.Identity.y].GeometryData.z != 0u;
        const float rawCosineAtLight = dot(
            light.NormalArea.xyz,
            -incomingDirection);
        const float cosineAtLight = twoSided
            ? abs(rawCosineAtLight)
            : rawCosineAtLight;
        if (cosineAtLight <= 0.0f || light.NormalArea.w <= 0.0f)
            return 0.0f;
        return selectionPdf * distanceSquared /
            max(light.NormalArea.w * cosineAtLight, 1e-20f);
    }
    return 0.0f;
}

float3 EstimateDirect(
    inout RandomSequence randomSequence,
    float3 position,
    float3 geometricNormal,
    float3 shadingNormal,
    float3 diffuseAlbedo,
    float3 throughput,
    bool hasBsdfCompetitor)
{
    float3 lightDirection;
    float lightDistance;
    float3 lightRadiance;
    float lightPdf;
    if (!SampleLight(
            randomSequence,
            position,
            lightDirection,
            lightDistance,
            lightRadiance,
            lightPdf))
        return 0.0f;
    const float cosineAtSurface = max(dot(shadingNormal, lightDirection), 0.0f);
    if (cosineAtSurface <= 0.0f ||
        dot(geometricNormal, lightDirection) <= 0.0f)
        return 0.0f;
    const float offset = RayOffset(position);
    const HitPayload shadow = TraceScene(
        position + geometricNormal * offset,
        lightDirection,
        max(lightDistance - 2.0f * offset, 0.0f));
    if (shadow.Hit != 0u)
        return 0.0f;
    const float bsdfPdf = cosineAtSurface / c_Pi;
    const float misWeight = hasBsdfCompetitor
        ? PowerHeuristic(lightPdf, bsdfPdf)
        : 1.0f;
    return throughput * (diffuseAlbedo / c_Pi) * lightRadiance *
        cosineAtSurface * misWeight /
        max(lightPdf, 1e-20f);
}

[shader("miss")]
void RealtimeMiss(inout HitPayload payload)
{
    payload.Hit = 0u;
}

[shader("closesthit")]
void RealtimeClosestHit(
    inout HitPayload payload,
    in BuiltInTriangleIntersectionAttributes attributes)
{
    const uint stableInstanceId = InstanceID();
    const RayTracingInstanceRecord instance = g_Instances[stableInstanceId];
    const uint geometryIndex = instance.Indices.x;
    const RayTracingGeometryRecord geometry = g_Geometries[geometryIndex];
    const uint primitiveIndex = PrimitiveIndex();
    const uint firstIndex = geometry.VertexIndexData.y + primitiveIndex * 3u;
    const uint index0 = g_Indices.Load((firstIndex + 0u) * 4u);
    const uint index1 = g_Indices.Load((firstIndex + 1u) * 4u);
    const uint index2 = g_Indices.Load((firstIndex + 2u) * 4u);
    const uint firstVertex = geometry.VertexIndexData.x;
    const RayTracingVertex vertex0 = g_Vertices[firstVertex + index0];
    const RayTracingVertex vertex1 = g_Vertices[firstVertex + index1];
    const RayTracingVertex vertex2 = g_Vertices[firstVertex + index2];
    const float3 barycentrics = float3(
        1.0f - attributes.barycentrics.x - attributes.barycentrics.y,
        attributes.barycentrics.x,
        attributes.barycentrics.y);
    const float3 position0 = mul(instance.World, vertex0.Position).xyz;
    const float3 position1 = mul(instance.World, vertex1.Position).xyz;
    const float3 position2 = mul(instance.World, vertex2.Position).xyz;
    float3 geometricNormal = SafeNormalize(
        cross(position1 - position0, position2 - position0),
        float3(0.0f, 1.0f, 0.0f));
    float3 shadingNormal = SafeNormalize(
        mul(
            (float3x3)instance.WorldInverseTranspose,
            vertex0.Normal.xyz * barycentrics.x +
            vertex1.Normal.xyz * barycentrics.y +
            vertex2.Normal.xyz * barycentrics.z),
        geometricNormal);
    if (dot(geometricNormal, WorldRayDirection()) > 0.0f)
        geometricNormal = -geometricNormal;
    if (dot(shadingNormal, geometricNormal) < 0.0f)
        shadingNormal = -shadingNormal;
    payload.Position =
        position0 * barycentrics.x +
        position1 * barycentrics.y +
        position2 * barycentrics.z;
    payload.RayT = RayTCurrent();
    payload.GeometricNormal = geometricNormal;
    payload.MaterialIndex = instance.Indices.y;
    payload.ShadingNormal = shadingNormal;
    payload.InstanceIndex = stableInstanceId;
    payload.GeometryIndex = geometryIndex;
    payload.PrimitiveIndex = primitiveIndex;
    payload.Hit = 1u;
    payload.FrontFace = HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE ? 1u : 0u;
}

[shader("raygeneration")]
void RealtimeRayGeneration()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 traceDimensions = g_DimensionsFrameBounces.xy;
    if (any(pixel >= traceDimensions))
        return;
    uint surfaceWidth = 0u;
    uint surfaceHeight = 0u;
    g_SurfaceDepth.GetDimensions(surfaceWidth, surfaceHeight);
    const uint2 surfacePixel = min(
        uint2((float2(pixel) + 0.5f) *
            float2(surfaceWidth, surfaceHeight) / float2(traceDimensions)),
        uint2(surfaceWidth - 1u, surfaceHeight - 1u));
    const float depth = g_SurfaceDepth.Load(int3(surfacePixel, 0));
    if (!isfinite(depth) || depth >= 1.0f)
    {
        g_NoisyDiffuse[pixel] = 0.0f;
        g_RawDirectDiffuse[pixel] = 0.0f;
        g_RawIndirectDiffuse[pixel] = 0.0f;
        g_HitDistance[pixel] = 0.0f;
        g_Confidence[pixel] = 0.0f;
        return;
    }

    const float2 uv = (float2(surfacePixel) + 0.5f) /
        float2(surfaceWidth, surfaceHeight);
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 worldPosition = mul(
        g_InverseViewProjection,
        float4(ndc, depth, 1.0f));
    worldPosition.xyz /= max(abs(worldPosition.w), 1e-20f);
    const float4 normalRoughness = g_SurfaceNormalRoughness.Load(
        int3(surfacePixel, 0));
    const float4 baseColorMetalness = g_SurfaceBaseColorMetalness.Load(
        int3(surfacePixel, 0));
    const float3 primaryNormal = SafeNormalize(
        normalRoughness.xyz,
        float3(0.0f, 1.0f, 0.0f));
    const float3 primaryAlbedo = max(baseColorMetalness.rgb, 0.0f) *
        (1.0f - saturate(baseColorMetalness.w)) *
        (g_SurfaceEmissive.Load(int3(surfacePixel, 0)).a < 0.5f ? 1.0f : 0.0f);

    RandomSequence randomSequence;
    randomSequence.SampleIndex = g_DimensionsFrameBounces.z;
    randomSequence.Dimension = 0u;
    randomSequence.Scramble = PcgHash(
        pixel.x + pixel.y * traceDimensions.x +
        g_SamplingSettings.x * 0x85ebca6bu);
    float3 directRadiance = EstimateDirect(
        randomSequence,
        worldPosition.xyz,
        primaryNormal,
        primaryNormal,
        primaryAlbedo,
        1.0f,
        g_DimensionsFrameBounces.w > 0u);
    float3 indirectRadiance = 0.0f;
    float3 throughput = primaryAlbedo;
    float bsdfPdf = 0.0f;
    float3 rayDirection = SampleCosineHemisphere(
        primaryNormal,
        float2(RandomFloat(randomSequence), RandomFloat(randomSequence)),
        bsdfPdf);
    if (dot(primaryNormal, rayDirection) <= 0.0f)
    {
        g_NoisyDiffuse[pixel] = float4(directRadiance, 1.0f);
        g_RawDirectDiffuse[pixel] = float4(directRadiance, 1.0f);
        g_RawIndirectDiffuse[pixel] = 0.0f;
        g_HitDistance[pixel] = 0.0f;
        g_Confidence[pixel] = 1.0f;
        return;
    }
    float3 rayOrigin = worldPosition.xyz +
        primaryNormal * RayOffset(worldPosition.xyz);
    float3 previousPosition = worldPosition.xyz;
    float firstHitDistance = g_CameraPositionAndMaxDistance.w;

    [loop]
    for (uint bounce = 0u; bounce < g_DimensionsFrameBounces.w; ++bounce)
    {
        const HitPayload hit = TraceScene(
            rayOrigin,
            rayDirection,
            g_CameraPositionAndMaxDistance.w);
        if (bounce == 0u)
            firstHitDistance = hit.Hit != 0u ? hit.RayT : g_CameraPositionAndMaxDistance.w;
        if (hit.Hit == 0u)
        {
            indirectRadiance += throughput * g_EnvironmentRadiance.rgb;
            break;
        }
        const RayTracingMaterialRecord material = g_Materials[hit.MaterialIndex];
        const float3 emissive = max(material.EmissiveRoughness.rgb, 0.0f);
        if (any(emissive > 0.0f))
        {
            const bool twoSided =
                g_Geometries[hit.GeometryIndex].GeometryData.z != 0u;
            if (hit.FrontFace != 0u || twoSided)
            {
                const float lightPdf = EmissiveHitSolidAnglePdf(
                    hit,
                    previousPosition,
                    rayDirection);
                const float misWeight = lightPdf > 0.0f
                    ? PowerHeuristic(bsdfPdf, lightPdf)
                    : 1.0f;
                indirectRadiance += throughput * emissive * misWeight;
            }
        }
        const float3 albedo = max(material.BaseColorMetallic.rgb, 0.0f) *
            (1.0f - saturate(material.BaseColorMetallic.w));
        indirectRadiance += EstimateDirect(
                randomSequence,
            hit.Position,
            hit.GeometricNormal,
            hit.ShadingNormal,
            albedo,
            throughput,
            bounce + 1u < g_DimensionsFrameBounces.w);
        if (bounce + 1u >= g_DimensionsFrameBounces.w)
            break;
        throughput *= albedo;
        if (bounce >= 1u)
        {
            const float survivalProbability = clamp(
                max(throughput.r, max(throughput.g, throughput.b)),
                0.05f,
                0.95f);
            if (RandomFloat(randomSequence) >= survivalProbability)
                break;
            throughput /= survivalProbability;
        }
        rayDirection = SampleCosineHemisphere(
            hit.ShadingNormal,
            float2(RandomFloat(randomSequence), RandomFloat(randomSequence)),
            bsdfPdf);
        if (dot(hit.GeometricNormal, rayDirection) <= 0.0f)
            break;
        previousPosition = hit.Position;
        rayOrigin = hit.Position +
            hit.GeometricNormal * RayOffset(hit.Position);
    }

    g_NoisyDiffuse[pixel] = float4(
        directRadiance + indirectRadiance,
        1.0f);
    g_RawDirectDiffuse[pixel] = float4(directRadiance, 1.0f);
    g_RawIndirectDiffuse[pixel] = float4(indirectRadiance, 1.0f);
    g_HitDistance[pixel] = firstHitDistance;
    g_Confidence[pixel] = 1.0f;
}
