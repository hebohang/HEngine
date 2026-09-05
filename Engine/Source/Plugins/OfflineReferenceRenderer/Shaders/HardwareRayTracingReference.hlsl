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

struct ReferencePathTracerConstants
{
    float4x4 g_InverseViewProjection;
    // xyz: camera position, w: maximum ray distance.
    float4 g_CameraPositionAndMaxDistance;
    // xy: output dimensions, z: sample index, w: maximum diffuse bounces.
    uint4 g_DimensionsSampleBounces;
    // x: fixed seed, y: direct-only, z: emissive triangle count,
    // w: sample scene environment cubemap.
    uint4 g_SamplingSettings;
    float4 g_EnvironmentRadiance;
};
HE_DECLARE_PUSH_CONSTANTS(
    ReferencePathTracerConstants,
    g_ReferencePathTracerConstants,
    space0);

#define g_InverseViewProjection \
    g_ReferencePathTracerConstants.g_InverseViewProjection
#define g_CameraPositionAndMaxDistance \
    g_ReferencePathTracerConstants.g_CameraPositionAndMaxDistance
#define g_DimensionsSampleBounces \
    g_ReferencePathTracerConstants.g_DimensionsSampleBounces
#define g_SamplingSettings \
    g_ReferencePathTracerConstants.g_SamplingSettings
#define g_EnvironmentRadiance \
    g_ReferencePathTracerConstants.g_EnvironmentRadiance

RaytracingAccelerationStructure g_SceneTlas : register(t0, space2);
StructuredBuffer<RayTracingVertex> g_Vertices : register(t1, space2);
ByteAddressBuffer g_Indices : register(t2, space2);
StructuredBuffer<RayTracingGeometryRecord> g_Geometries : register(t3, space2);
StructuredBuffer<RayTracingMaterialRecord> g_Materials : register(t4, space2);
StructuredBuffer<RayTracingInstanceRecord> g_Instances : register(t5, space2);
StructuredBuffer<RayTracingEmissiveTriangle> g_EmissiveTriangles : register(t6, space2);

TextureCube<float4> g_EnvironmentMap : register(t0, space4);
SamplerState g_EnvironmentSampler : register(s0, space4);

RWTexture2D<float4> g_AccumulatedRadiance : register(u0, space3);
RWTexture2D<float4> g_DirectDiffuse : register(u1, space3);
RWTexture2D<float4> g_IndirectDiffuse : register(u2, space3);
RWTexture2D<float4> g_Albedo : register(u3, space3);
RWTexture2D<float4> g_WorldNormal : register(u4, space3);
RWTexture2D<float> g_ViewDepth : register(u5, space3);
RWTexture2D<uint> g_MaterialId : register(u6, space3);
RWTexture2D<uint> g_InstanceId : register(u7, space3);
RWTexture2D<uint> g_SampleCount : register(u8, space3);

uint PcgHash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float RandomFloat(inout uint state)
{
    state = PcgHash(state);
    return (float(state) + 0.5f) * (1.0f / 4294967296.0f);
}

float3 SafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return all(isfinite(value)) && isfinite(lengthSquared) && lengthSquared > 1e-20f
        ? value * rsqrt(lengthSquared)
        : fallback;
}

float MaxAbsComponent(float3 value)
{
    return max(abs(value.x), max(abs(value.y), abs(value.z)));
}

float RayOffset(float3 position)
{
    return max(1e-5f, 2e-5f * max(MaxAbsComponent(position), 1.0f));
}

float PowerHeuristic(float firstPdf, float secondPdf)
{
    const float firstSquared = firstPdf * firstPdf;
    const float secondSquared = secondPdf * secondPdf;
    return firstSquared / max(firstSquared + secondSquared, 1e-20f);
}

float3 BuildTangent(float3 normal)
{
    return abs(normal.z) < 0.999f
        ? SafeNormalize(cross(float3(0.0f, 0.0f, 1.0f), normal), float3(1.0f, 0.0f, 0.0f))
        : SafeNormalize(cross(float3(0.0f, 1.0f, 0.0f), normal), float3(1.0f, 0.0f, 0.0f));
}

float3 SampleCosineHemisphere(float3 normal, float2 randomValue, out float pdf)
{
    const float radius = sqrt(randomValue.x);
    const float angle = c_TwoPi * randomValue.y;
    float sineValue = 0.0f;
    float cosineValue = 1.0f;
    sincos(angle, sineValue, cosineValue);
    const float3 localDirection = float3(
        radius * cosineValue,
        radius * sineValue,
        sqrt(max(1.0f - randomValue.x, 0.0f)));
    const float3 tangent = BuildTangent(normal);
    const float3 bitangent = cross(normal, tangent);
    const float3 direction = SafeNormalize(
        tangent * localDirection.x +
        bitangent * localDirection.y +
        normal * localDirection.z,
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

bool SampleEmissiveTriangle(
    inout uint randomState,
    float3 shadingPosition,
    out float3 direction,
    out float distanceToLight,
    out float3 radiance,
    out float solidAnglePdf)
{
    direction = 0.0f;
    distanceToLight = 0.0f;
    radiance = 0.0f;
    solidAnglePdf = 0.0f;
    const uint lightCount = g_SamplingSettings.z;
    if (lightCount == 0u)
        return false;

    const float selectionSample = RandomFloat(randomState);
    uint selectedIndex = lightCount - 1u;
    float previousCdf = 0.0f;
    [loop]
    for (uint lightIndex = 0u; lightIndex < lightCount; ++lightIndex)
    {
        const float cdf = g_EmissiveTriangles[lightIndex].RadianceCdf.w;
        if (selectionSample <= cdf)
        {
            selectedIndex = lightIndex;
            break;
        }
        previousCdf = cdf;
    }

    const RayTracingEmissiveTriangle light = g_EmissiveTriangles[selectedIndex];
    const float selectionPdf = max(light.RadianceCdf.w - previousCdf, 0.0f);
    const float root = sqrt(RandomFloat(randomState));
    const float second = RandomFloat(randomState);
    const float barycentric0 = 1.0f - root;
    const float barycentric1 = root * (1.0f - second);
    const float barycentric2 = root * second;
    const float3 lightPosition =
        light.Position0.xyz * barycentric0 +
        light.Position1.xyz * barycentric1 +
        light.Position2.xyz * barycentric2;
    const float3 toLight = lightPosition - shadingPosition;
    const float distanceSquared = dot(toLight, toLight);
    if (!isfinite(distanceSquared) || distanceSquared <= 1e-16f)
        return false;
    distanceToLight = sqrt(distanceSquared);
    direction = toLight / distanceToLight;
    const bool twoSided =
        g_Geometries[light.Identity.y].GeometryData.z != 0u;
    const float rawCosineAtLight = dot(light.NormalArea.xyz, -direction);
    const float cosineAtLight = twoSided
        ? abs(rawCosineAtLight)
        : rawCosineAtLight;
    if (cosineAtLight <= 0.0f || light.NormalArea.w <= 0.0f || selectionPdf <= 0.0f)
        return false;
    solidAnglePdf = selectionPdf * distanceSquared /
        max(light.NormalArea.w * cosineAtLight, 1e-20f);
    radiance = light.RadianceCdf.rgb;
    return solidAnglePdf > 0.0f && all(isfinite(radiance));
}

float EmissiveHitSolidAnglePdf(
    HitPayload hit,
    float3 previousPosition,
    float3 incomingDirection)
{
    const uint lightCount = g_SamplingSettings.z;
    float previousCdf = 0.0f;
    [loop]
    for (uint lightIndex = 0u; lightIndex < lightCount; ++lightIndex)
    {
        const RayTracingEmissiveTriangle light = g_EmissiveTriangles[lightIndex];
        const float selectionPdf = max(light.RadianceCdf.w - previousCdf, 0.0f);
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

[shader("miss")]
void ReferenceMiss(inout HitPayload payload)
{
    payload.Hit = 0u;
}

[shader("closesthit")]
void ReferenceClosestHit(
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

    const float3 worldPosition0 = mul(instance.World, vertex0.Position).xyz;
    const float3 worldPosition1 = mul(instance.World, vertex1.Position).xyz;
    const float3 worldPosition2 = mul(instance.World, vertex2.Position).xyz;
    const float3 worldPosition =
        worldPosition0 * barycentrics.x +
        worldPosition1 * barycentrics.y +
        worldPosition2 * barycentrics.z;
    float3 geometricNormal = SafeNormalize(
        cross(worldPosition1 - worldPosition0, worldPosition2 - worldPosition0),
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

    payload.Position = worldPosition;
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
void ReferenceRayGeneration()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = g_DimensionsSampleBounces.xy;
    if (any(pixel >= dimensions))
        return;

    const uint sampleIndex = g_DimensionsSampleBounces.z;
    uint randomState = PcgHash(
        pixel.x + pixel.y * dimensions.x +
        sampleIndex * 0x9e3779b9u +
        g_SamplingSettings.x * 0x85ebca6bu);
    const float2 subpixel = float2(
        RandomFloat(randomState),
        RandomFloat(randomState));
    const float2 uv = (float2(pixel) + subpixel) / float2(dimensions);
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 farPosition = mul(
        g_InverseViewProjection,
        float4(ndc, 1.0f, 1.0f));
    farPosition.xyz /= max(abs(farPosition.w), 1e-20f);
    float3 rayOrigin = g_CameraPositionAndMaxDistance.xyz;
    float3 rayDirection = SafeNormalize(
        farPosition.xyz - rayOrigin,
        float3(0.0f, 0.0f, 1.0f));
    float4 cameraFarPosition = mul(
        g_InverseViewProjection,
        float4(0.0f, 0.0f, 1.0f, 1.0f));
    cameraFarPosition.xyz /= max(abs(cameraFarPosition.w), 1e-20f);
    const float3 cameraForward = SafeNormalize(
        cameraFarPosition.xyz - rayOrigin,
        rayDirection);

    float3 throughput = 1.0f;
    float3 directDiffuse = 0.0f;
    float3 indirectDiffuse = 0.0f;
    float3 firstAlbedo = 0.0f;
    float3 firstNormal = 0.0f;
    float firstDepth = 0.0f;
    uint firstMaterial = c_InvalidIndex;
    uint firstInstance = c_InvalidIndex;
    float previousBsdfPdf = 0.0f;
    float3 previousPosition = rayOrigin;
    const uint maximumBounces = g_DimensionsSampleBounces.w;

    [loop]
    for (uint bounce = 0u; bounce <= maximumBounces; ++bounce)
    {
        const HitPayload hit = TraceScene(
            rayOrigin,
            rayDirection,
            g_CameraPositionAndMaxDistance.w);
        if (hit.Hit == 0u)
        {
            float3 environmentRadiance = g_EnvironmentRadiance.rgb;
            if (g_SamplingSettings.w != 0u)
            {
                environmentRadiance *= max(
                    g_EnvironmentMap.SampleLevel(
                        g_EnvironmentSampler,
                        rayDirection,
                        0.0f).rgb,
                    0.0f);
            }
            const float3 missContribution = throughput * environmentRadiance;
            if (bounce == 0u)
                directDiffuse += missContribution;
            else
                indirectDiffuse += missContribution;
            break;
        }

        const RayTracingMaterialRecord material = g_Materials[hit.MaterialIndex];
        const float3 diffuseAlbedo = max(material.BaseColorMetallic.rgb, 0.0f) *
            (1.0f - saturate(material.BaseColorMetallic.w));
        const float3 emissiveRadiance = max(material.EmissiveRoughness.rgb, 0.0f);
        if (bounce == 0u)
        {
            firstAlbedo = diffuseAlbedo;
            firstNormal = hit.ShadingNormal;
            firstDepth = max(dot(
                hit.Position - g_CameraPositionAndMaxDistance.xyz,
                cameraForward), 0.0f);
            firstMaterial = hit.MaterialIndex;
            firstInstance = hit.InstanceIndex;
        }

        if (any(emissiveRadiance > 0.0f))
        {
            const bool twoSided =
                g_Geometries[hit.GeometryIndex].GeometryData.z != 0u;
            if (hit.FrontFace == 0u && !twoSided)
                break;
            float misWeight = 1.0f;
            if (bounce > 0u)
            {
                const float lightPdf = EmissiveHitSolidAnglePdf(
                    hit,
                    previousPosition,
                    rayDirection);
                if (lightPdf > 0.0f)
                    misWeight = PowerHeuristic(previousBsdfPdf, lightPdf);
            }
            const float3 emission = throughput * emissiveRadiance * misWeight;
            if (bounce == 0u)
                directDiffuse += emission;
            else
                indirectDiffuse += emission;
        }

        float3 lightDirection = 0.0f;
        float lightDistance = 0.0f;
        float3 lightRadiance = 0.0f;
        float lightPdf = 0.0f;
        if (SampleEmissiveTriangle(
                randomState,
                hit.Position,
                lightDirection,
                lightDistance,
                lightRadiance,
                lightPdf))
        {
            const float cosineAtSurface = max(
                dot(hit.ShadingNormal, lightDirection),
                0.0f);
            if (cosineAtSurface > 0.0f &&
                dot(hit.GeometricNormal, lightDirection) > 0.0f)
            {
                const float offset = RayOffset(hit.Position);
                const HitPayload shadowHit = TraceScene(
                    hit.Position + hit.GeometricNormal * offset,
                    lightDirection,
                    max(lightDistance - 2.0f * offset, 0.0f));
                if (shadowHit.Hit == 0u)
                {
                    const float bsdfPdf = cosineAtSurface / c_Pi;
                    const bool hasBsdfCompetitor =
                        g_SamplingSettings.y == 0u &&
                        bounce < maximumBounces;
                    const float misWeight = hasBsdfCompetitor
                        ? PowerHeuristic(lightPdf, bsdfPdf)
                        : 1.0f;
                    const float3 directSample = throughput *
                        (diffuseAlbedo / c_Pi) * lightRadiance *
                        cosineAtSurface * misWeight / max(lightPdf, 1e-20f);
                    if (bounce == 0u)
                        directDiffuse += directSample;
                    else
                        indirectDiffuse += directSample;
                }
            }
        }

        if (g_SamplingSettings.y != 0u || bounce >= maximumBounces)
            break;

        previousPosition = hit.Position;
        rayDirection = SampleCosineHemisphere(
            hit.ShadingNormal,
            float2(RandomFloat(randomState), RandomFloat(randomState)),
            previousBsdfPdf);
        if (previousBsdfPdf <= 0.0f)
            break;
        if (dot(hit.GeometricNormal, rayDirection) <= 0.0f)
            break;
        throughput *= diffuseAlbedo;
        if (bounce >= 2u)
        {
            const float survivalProbability = clamp(
                max(throughput.r, max(throughput.g, throughput.b)),
                0.05f,
                0.95f);
            if (RandomFloat(randomState) >= survivalProbability)
                break;
            throughput /= survivalProbability;
        }
        const float offset = RayOffset(hit.Position);
        rayOrigin = hit.Position + hit.GeometricNormal * offset;
    }

    const float3 sampleRadiance = directDiffuse + indirectDiffuse;
    if (sampleIndex == 0u)
    {
        g_AccumulatedRadiance[pixel] = float4(sampleRadiance, 1.0f);
        g_DirectDiffuse[pixel] = float4(directDiffuse, 1.0f);
        g_IndirectDiffuse[pixel] = float4(indirectDiffuse, 1.0f);
    }
    else
    {
        const float previousWeight = float(sampleIndex);
        const float reciprocalCount = 1.0f / float(sampleIndex + 1u);
        g_AccumulatedRadiance[pixel] = float4(
            (g_AccumulatedRadiance[pixel].rgb * previousWeight + sampleRadiance) *
                reciprocalCount,
            1.0f);
        g_DirectDiffuse[pixel] = float4(
            (g_DirectDiffuse[pixel].rgb * previousWeight + directDiffuse) *
                reciprocalCount,
            1.0f);
        g_IndirectDiffuse[pixel] = float4(
            (g_IndirectDiffuse[pixel].rgb * previousWeight + indirectDiffuse) *
                reciprocalCount,
            1.0f);
    }
    g_Albedo[pixel] = float4(firstAlbedo, 1.0f);
    g_WorldNormal[pixel] = float4(firstNormal, 0.0f);
    g_ViewDepth[pixel] = firstDepth;
    g_MaterialId[pixel] = firstMaterial;
    g_InstanceId[pixel] = firstInstance;
    g_SampleCount[pixel] = sampleIndex + 1u;
}
