#define HE_TEXTURE_SAMPLING_NO_DERIVATIVES
// Reference rays use the same decoded-texel arithmetic for every sampler mode,
// including defaults, independent of native sRGB filtering approximation.
#define HE_TEXTURE_SAMPLING_EXPLICIT_FILTER
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
    float4 TexCoord;
    float4 Color;
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
struct AnalyticLight { float4 PositionType; float4 DirectionRadius; float4 Radiance; float4 Cone; };
StructuredBuffer<AnalyticLight> g_AnalyticLights : register(t7, space2);
Texture2D<float4> g_MaterialTextures[512] : register(t8, space2);
SamplerState g_MaterialSampler : register(s0, space2);

float4 SampleMaterialTexture(uint index, float4 uv, HETextureSampling sampling, float4 fallback)
{
    if (index == c_InvalidIndex) return fallback;
    // Fixed mip zero keeps reference transport independent of scheduling.
    // Primary-ray jitter integrates the pixel footprint across samples.
    return HE_SampleTextureLod(g_MaterialTextures[NonUniformResourceIndex(index)], g_MaterialSampler, sampling, HE_TextureUv(sampling, uv.xy, uv.zw), 0);
}

RayTracingMaterialRecord EvaluateMaterial(uint index, float4 uv, float4 color)
{
    RayTracingMaterialRecord material = g_Materials[index];
    material.BaseColorMetallic.rgb *= SampleMaterialTexture(material.Textures0.x, uv, material.Sampling[0], 1).rgb * color.rgb;
    material.Surface.y *= color.a;
    const float4 mr = SampleMaterialTexture(material.Textures0.z, uv, material.Sampling[2], 1);
    material.BaseColorMetallic.w = saturate(material.BaseColorMetallic.w * mr.b * SampleMaterialTexture(material.Textures1.y, uv, material.Sampling[5], 1).r);
    material.EmissiveRoughness.w = clamp(material.EmissiveRoughness.w * mr.g * SampleMaterialTexture(material.Textures1.z, uv, material.Sampling[6], 1).r, 0.04f, 1.0f);
    material.EmissiveRoughness.rgb *= SampleMaterialTexture(material.Textures1.x, uv, material.Sampling[4], 1).rgb;
    return material;
}

float4 TriangleUv(uint geometryIndex, uint primitiveIndex, float3 barycentrics)
{
    const RayTracingGeometryRecord geometry = g_Geometries[geometryIndex];
    const uint first = geometry.VertexIndexData.y + primitiveIndex * 3u;
    return g_Vertices[geometry.VertexIndexData.x + g_Indices.Load(first * 4u)].TexCoord * barycentrics.x +
        g_Vertices[geometry.VertexIndexData.x + g_Indices.Load((first + 1u) * 4u)].TexCoord * barycentrics.y +
        g_Vertices[geometry.VertexIndexData.x + g_Indices.Load((first + 2u) * 4u)].TexCoord * barycentrics.z;
}

float4 TriangleColor(uint geometryIndex, uint primitiveIndex, float3 barycentrics)
{
    const RayTracingGeometryRecord geometry = g_Geometries[geometryIndex];
    const uint first = geometry.VertexIndexData.y + primitiveIndex * 3u;
    return g_Vertices[geometry.VertexIndexData.x + g_Indices.Load(first * 4u)].Color * barycentrics.x +
        g_Vertices[geometry.VertexIndexData.x + g_Indices.Load((first + 1u) * 4u)].Color * barycentrics.y +
        g_Vertices[geometry.VertexIndexData.x + g_Indices.Load((first + 2u) * 4u)].Color * barycentrics.z;
}

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

float3 FresnelSchlick(float cosine, float3 f0)
{
    return f0 + (1.0f - f0) * pow(saturate(1.0f - cosine), 5.0f);
}

float GgxD(float noH, float alpha)
{
    const float a2 = alpha * alpha;
    const float d = noH * noH * (a2 - 1.0f) + 1.0f;
    return a2 / max(c_Pi * d * d, 1e-20f);
}

float GgxG1(float noV, float alpha)
{
    return 2.0f * noV / max(noV + sqrt(alpha * alpha + (1.0f - alpha * alpha) * noV * noV), 1e-20f);
}

// Evaluate and sampling share this exact mixture density, including in MIS.
float3 EvaluateBsdf(RayTracingMaterialRecord material, float3 n, float3 wo, float3 wi, out float pdf)
{
    pdf = 0.0f;
    const float noV = dot(n, wo), noL = dot(n, wi);
    if (noV <= 0.0f || noL <= 0.0f) return 0.0f;
    const float3 h = SafeNormalize(wo + wi, n);
    const float noH = saturate(dot(n, h)), voH = saturate(dot(wo, h));
    const float metal = saturate(material.BaseColorMetallic.w);
    const float3 base = saturate(material.BaseColorMetallic.rgb);
    const float alpha = max(0.001f, material.EmissiveRoughness.w * material.EmissiveRoughness.w);
    const float d = GgxD(noH, alpha);
    const float3 f = FresnelSchlick(voH, lerp(0.04f.xxx, base, metal));
    const float pSpecular = 0.5f + 0.5f * metal;
    pdf = lerp(noL / c_Pi, d * noH / max(4.0f * voH, 1e-20f), pSpecular);
    return (1.0f - metal) * base * (1.0f - f) / c_Pi +
        f * d * GgxG1(noV, alpha) * GgxG1(noL, alpha) / max(4.0f * noV * noL, 1e-20f);
}

float3 SampleBsdf(RayTracingMaterialRecord material, float3 n, float3 wo, inout uint rng, out float pdf, out float3 weight)
{
    const float select = RandomFloat(rng);
    const float2 u = float2(RandomFloat(rng), RandomFloat(rng));
    float3 wi;
    if (select < 0.5f + 0.5f * saturate(material.BaseColorMetallic.w))
    {
        const float alpha = max(0.001f, material.EmissiveRoughness.w * material.EmissiveRoughness.w);
        const float cosTheta = sqrt((1.0f - u.x) / max(1.0f + (alpha * alpha - 1.0f) * u.x, 1e-20f));
        const float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
        const float3 tangent = BuildTangent(n);
        const float3 h = tangent * (sinTheta * cos(c_TwoPi * u.y)) +
            cross(n, tangent) * (sinTheta * sin(c_TwoPi * u.y)) + n * cosTheta;
        wi = reflect(-wo, h);
    }
    else wi = SampleCosineHemisphere(n, u, pdf);
    const float3 f = EvaluateBsdf(material, n, wo, wi, pdf);
    weight = f * max(dot(n, wi), 0.0f) / max(pdf, 1e-20f);
    return wi;
}

float3 EvaluateEnvironment(float3 direction)
{
    float3 radiance = g_EnvironmentRadiance.rgb;
    if (g_SamplingSettings.w != 0u)
        radiance *= max(g_EnvironmentMap.SampleLevel(g_EnvironmentSampler, direction, 0.0f).rgb, 0.0f);
    return radiance;
}

bool IsOccluded(float3 origin, float3 direction, float maximumDistance)
{
    if (maximumDistance <= 0) return false;
    RayDesc ray;
    ray.Origin = origin; ray.Direction = direction; ray.TMin = 0; ray.TMax = maximumDistance;
    HitPayload payload = (HitPayload)0;
    payload.Hit = 1u;
    // Retain alpha any-hit, but skip full normal/material evaluation for shadows.
    TraceRay(g_SceneTlas, RAY_FLAG_CULL_BACK_FACING_TRIANGLES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
        RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xff, 0, 1, 0, ray, payload);
    return payload.Hit != 0u;
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
    uint lower = 0u, upper = lightCount - 1u;
    [loop]
    while (lower < upper)
    {
        const uint middle = lower + (upper - lower) / 2u;
        if (selectionSample <= g_EmissiveTriangles[middle].RadianceCdf.w) upper = middle;
        else lower = middle + 1u;
    }
    const uint selectedIndex = lower;
    const float previousCdf = selectedIndex ? g_EmissiveTriangles[selectedIndex - 1u].RadianceCdf.w : 0.0f;

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
    const float4 uv = TriangleUv(light.Identity.y, light.Identity.z, float3(barycentric0, barycentric1, barycentric2));
    const RayTracingMaterialRecord material = EvaluateMaterial(light.Identity.w, uv, TriangleColor(light.Identity.y, light.Identity.z, float3(barycentric0, barycentric1, barycentric2)));
    radiance = material.EmissiveRoughness.rgb;
    if (material.Surface.w > 0.5f && SampleMaterialTexture(material.Textures0.x, uv, material.Sampling[0], 1).a * material.Surface.y < material.Surface.z)
        radiance = 0;
    return solidAnglePdf > 0.0f && all(isfinite(radiance));
}

float EmissiveHitSolidAnglePdf(
    HitPayload hit,
    float3 previousPosition,
    float3 incomingDirection)
{
    const uint lightCount = g_SamplingSettings.z;
    uint lower = 0u, upper = lightCount;
    while (lower < upper)
    {
        const uint middle = lower + (upper - lower) / 2u;
        const uint3 identity = g_EmissiveTriangles[middle].Identity.xyz;
        const bool before = identity.x < hit.InstanceIndex ||
            (identity.x == hit.InstanceIndex && (identity.y < hit.GeometryIndex ||
            (identity.y == hit.GeometryIndex && identity.z < hit.PrimitiveIndex)));
        if (before) lower = middle + 1u;
        else upper = middle;
    }
    if (lower < lightCount)
    {
        const uint lightIndex = lower;
        const RayTracingEmissiveTriangle light = g_EmissiveTriangles[lightIndex];
        const float previousCdf = lightIndex > 0u ? g_EmissiveTriangles[lightIndex - 1u].RadianceCdf.w : 0.0f;
        const float selectionPdf = max(light.RadianceCdf.w - previousCdf, 0.0f);
        if (light.Identity.x != hit.InstanceIndex ||
            light.Identity.y != hit.GeometryIndex ||
            light.Identity.z != hit.PrimitiveIndex)
        {
            return 0.0f;
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

[shader("anyhit")]
void ReferenceAnyHit(inout HitPayload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    const RayTracingInstanceRecord instance = g_Instances[InstanceID()];
    const RayTracingMaterialRecord material = g_Materials[instance.Indices.y];
    const float4 uv = TriangleUv(instance.Indices.x, PrimitiveIndex(),
        float3(1.0f - attributes.barycentrics.x - attributes.barycentrics.y, attributes.barycentrics));
    if (material.Surface.w > 0.5f && SampleMaterialTexture(material.Textures0.x, uv, material.Sampling[0], 1).a * material.Surface.y * TriangleColor(instance.Indices.x, PrimitiveIndex(),
        float3(1.0f - attributes.barycentrics.x - attributes.barycentrics.y, attributes.barycentrics)).a < material.Surface.z)
        IgnoreHit();
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
    const bool reverseFrame = dot(geometricNormal, WorldRayDirection()) > 0.0f;
    if (reverseFrame)
        geometricNormal = -geometricNormal;
    if (dot(shadingNormal, geometricNormal) < 0.0f)
        shadingNormal = -shadingNormal;
    const float4 uv = vertex0.TexCoord * barycentrics.x + vertex1.TexCoord * barycentrics.y + vertex2.TexCoord * barycentrics.z;
    const RayTracingMaterialRecord material = g_Materials[instance.Indices.y];
    if (material.Textures0.y != c_InvalidIndex)
    {
        float3 tangentRaw = mul((float3x3)instance.World,
            vertex0.Tangent.xyz * barycentrics.x + vertex1.Tangent.xyz * barycentrics.y + vertex2.Tangent.xyz * barycentrics.z);
        float3 bitangentRaw = mul((float3x3)instance.World,
            vertex0.Bitangent.xyz * barycentrics.x + vertex1.Bitangent.xyz * barycentrics.y + vertex2.Bitangent.xyz * barycentrics.z) * (reverseFrame ? -1.f : 1.f);
        const HETextureSampling normalSampling = material.Sampling[1];
        if (normalSampling.Options.x != 0 || any(normalSampling.UvRow0.xyz != float3(1,0,0)) || any(normalSampling.UvRow1.xyz != float3(0,1,0)))
        {
            float2 uv0 = HE_TextureUv(normalSampling, vertex0.TexCoord.xy, vertex0.TexCoord.zw);
            float2 du = HE_TextureUv(normalSampling, vertex1.TexCoord.xy, vertex1.TexCoord.zw) - uv0;
            float2 dv = HE_TextureUv(normalSampling, vertex2.TexCoord.xy, vertex2.TexCoord.zw) - uv0;
            float3 dp = mul(instance.World, vertex1.Position).xyz - mul(instance.World, vertex0.Position).xyz;
            float3 dq = mul(instance.World, vertex2.Position).xyz - mul(instance.World, vertex0.Position).xyz;
            float determinant = du.x * dv.y - du.y * dv.x;
            if (abs(determinant) > 1e-10) { tangentRaw = (dp * dv.y - dq * du.y) / determinant; bitangentRaw = (dq * du.x - dp * dv.x) / determinant; }
        }
        const float3 tangent = SafeNormalize(tangentRaw - shadingNormal * dot(tangentRaw, shadingNormal), BuildTangent(shadingNormal));
        const float handedness = dot(cross(shadingNormal, tangent), bitangentRaw) < 0 ? -1.0f : 1.0f;
        const float3 bitangent = cross(shadingNormal, tangent) * handedness;
        float3 mapped = SampleMaterialTexture(material.Textures0.y, uv, material.Sampling[1], float4(0.5f, 0.5f, 1.f, 1.f)).xyz * 2.f - 1.f;
        if (material.Textures1.w == 1u) mapped.z = sqrt(saturate(1.f - dot(mapped.xy, mapped.xy)));
        mapped.xy *= material.Surface.x;
        shadingNormal = SafeNormalize(tangent * mapped.x + bitangent * mapped.y + shadingNormal * mapped.z, shadingNormal);
    }
    if (dot(shadingNormal, geometricNormal) <= 0 || dot(shadingNormal, -WorldRayDirection()) <= 0)
        shadingNormal = geometricNormal;

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
    payload.TexCoord = uv;
    payload.Color = vertex0.Color * barycentrics.x + vertex1.Color * barycentrics.y + vertex2.Color * barycentrics.z;
}

[shader("raygeneration")]
void ReferenceRayGeneration()
{
    const uint2 dimensions = g_DimensionsSampleBounces.xy;
    const uint band = asuint(g_EnvironmentRadiance.w);
    const uint stride = max(1u, band >> 16u);
    const uint2 pixel = uint2(DispatchRaysIndex().x,
        ((DispatchRaysIndex().y + (band & 65535u)) * stride) % dimensions.y);
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
    // Preserve primary-ray arithmetic across differently sized dispatches.
    // Tiny fast-math direction differences change texture filtering and paths;
    // fixed pixel/seed/sample must produce the same reference after a restart.
    precise float4 farPosition = mul(
        g_InverseViewProjection,
        float4(ndc, 1.0f, 1.0f));
    farPosition.xyz /= max(abs(farPosition.w), 1e-20f);
    float3 rayOrigin = g_CameraPositionAndMaxDistance.xyz;
    precise float3 rayDirection = SafeNormalize(
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
            const float3 environmentRadiance = EvaluateEnvironment(rayDirection);
            const float3 missContribution = throughput * environmentRadiance *
                (bounce == 0u ? 1.0f : PowerHeuristic(previousBsdfPdf, 1.0f / c_TwoPi));
            if (bounce == 0u)
                directDiffuse += missContribution;
            else
                indirectDiffuse += missContribution;
            break;
        }

        const RayTracingMaterialRecord material = EvaluateMaterial(hit.MaterialIndex, hit.TexCoord, hit.Color);
        const float3 diffuseAlbedo = max(material.BaseColorMetallic.rgb, 0.0f) *
            (1.0f - saturate(material.BaseColorMetallic.w));
        const float3 emissiveRadiance = max(material.EmissiveRoughness.rgb, 0.0f);
        if (bounce == 0u)
        {
            firstAlbedo = saturate(material.BaseColorMetallic.rgb);
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
                const bool shadowHit = IsOccluded(
                    hit.Position + hit.GeometricNormal * offset,
                    lightDirection,
                    max(lightDistance - 2.0f * offset, 0.0f));
                if (!shadowHit)
                {
                    float bsdfPdf;
                    const float3 bsdf = EvaluateBsdf(material, hit.ShadingNormal, -rayDirection, lightDirection, bsdfPdf);
                    const bool hasBsdfCompetitor =
                        g_SamplingSettings.y == 0u &&
                        bounce < maximumBounces;
                    const float misWeight = hasBsdfCompetitor
                        ? PowerHeuristic(lightPdf, bsdfPdf)
                        : 1.0f;
                    const float3 directSample = throughput *
                        bsdf * lightRadiance *
                        cosineAtSurface * misWeight / max(lightPdf, 1e-20f);
                    if (bounce == 0u)
                        directDiffuse += directSample;
                    else
                        indirectDiffuse += directSample;
                }
            }
        }

        // Delta lights have no competing BSDF-hit strategy, so their MIS weight is one.
        const uint analyticCount = (uint)g_AnalyticLights[0].PositionType.x;
        for (uint lightIndex = 1u; lightIndex <= analyticCount; ++lightIndex)
        {
            const AnalyticLight light = g_AnalyticLights[lightIndex];
            float3 direction = SafeNormalize(-light.DirectionRadius.xyz, hit.GeometricNormal);
            float distance = g_CameraPositionAndMaxDistance.w;
            float attenuation = 1.0f;
            if (light.PositionType.w > 0.5f)
            {
                const float3 toLight = light.PositionType.xyz - hit.Position;
                distance = length(toLight);
                direction = toLight / max(distance, 1e-6f);
                const float normalizedDistance = distance / max(light.DirectionRadius.w, 1e-3f);
                const float falloff = saturate(1.0f - pow(normalizedDistance, 4.0f));
                attenuation = falloff * falloff / max(distance * distance, 1e-4f);
                if (light.PositionType.w > 1.5f)
                {
                    const float cone = saturate((dot(-direction, normalize(light.DirectionRadius.xyz)) - light.Cone.y) / max(light.Cone.x - light.Cone.y, 1e-5f));
                    attenuation *= cone * cone;
                }
            }
            const float cosine = max(dot(hit.ShadingNormal, direction), 0.0f);
            if (attenuation > 0 && cosine > 0 && dot(hit.GeometricNormal, direction) > 0)
            {
                const bool shadow = IsOccluded(hit.Position + hit.GeometricNormal * RayOffset(hit.Position), direction,
                    max(0.f, distance - 2.f * RayOffset(hit.Position)));
                if (!shadow)
                {
                    float pdf;
                    const float3 contribution = throughput * EvaluateBsdf(material, hit.ShadingNormal, -rayDirection, direction, pdf) *
                        light.Radiance.rgb * (attenuation * cosine);
                    if (bounce == 0u) directDiffuse += contribution;
                    else indirectDiffuse += contribution;
                }
            }
        }

        // Environment NEE also runs in DirectOnly. Uniform hemisphere sampling
        // has a known nonzero density over every environment texel direction.
        const float envZ = RandomFloat(randomState);
        const float envPhi = c_TwoPi * RandomFloat(randomState);
        const float envRadius = sqrt(max(0.0f, 1.0f - envZ * envZ));
        const float3 envTangent = BuildTangent(hit.ShadingNormal);
        const float3 envDirection = envTangent * (envRadius * cos(envPhi)) +
            cross(hit.ShadingNormal, envTangent) * (envRadius * sin(envPhi)) + hit.ShadingNormal * envZ;
        if (dot(hit.GeometricNormal, envDirection) > 0.0f)
        {
            float envBsdfPdf;
            const float3 envBsdf = EvaluateBsdf(material, hit.ShadingNormal, -rayDirection, envDirection, envBsdfPdf);
            const float envPdf = 1.0f / c_TwoPi;
            const float envMis = g_SamplingSettings.y == 0u && bounce < maximumBounces ? PowerHeuristic(envPdf, envBsdfPdf) : 1.0f;
            const bool envHit = IsOccluded(hit.Position + hit.GeometricNormal * RayOffset(hit.Position), envDirection, g_CameraPositionAndMaxDistance.w);
            if (!envHit)
            {
                const float3 contribution = throughput * envBsdf * EvaluateEnvironment(envDirection) * envZ * envMis / envPdf;
                if (bounce == 0u) directDiffuse += contribution;
                else indirectDiffuse += contribution;
            }
        }

        if (g_SamplingSettings.y != 0u || bounce >= maximumBounces)
            break;

        previousPosition = hit.Position;
        float3 bsdfWeight;
        rayDirection = SampleBsdf(material, hit.ShadingNormal, -rayDirection, randomState, previousBsdfPdf, bsdfWeight);
        if (previousBsdfPdf <= 0.0f)
            break;
        if (dot(hit.GeometricNormal, rayDirection) <= 0.0f)
            break;
        throughput *= bsdfWeight;
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
    // Pixel-filtered guides, using the identical primary samples as beauty.
    // A miss contributes zero; these are noisy guides (cleanAux must be false).
    const float guideWeight = 1.0f / float(sampleIndex + 1u);
    g_Albedo[pixel] = float4(sampleIndex == 0u ? firstAlbedo :
        lerp(g_Albedo[pixel].rgb, firstAlbedo, guideWeight), 1.0f);
    g_WorldNormal[pixel] = float4(sampleIndex == 0u ? firstNormal :
        lerp(g_WorldNormal[pixel].rgb, firstNormal, guideWeight), 0.0f);
    g_ViewDepth[pixel] = firstDepth;
    g_MaterialId[pixel] = firstMaterial;
    g_InstanceId[pixel] = firstInstance;
    g_SampleCount[pixel] = sampleIndex + 1u;
}
