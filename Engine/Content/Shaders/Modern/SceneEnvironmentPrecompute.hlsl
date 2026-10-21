#pragma htype cs CS

#include "Shared/BackendPushConstants.hlsli"

// Startup-only IBL precomputation. Output cubemaps are addressed as 2D arrays
// so every face/mip can be dispatched through the same immutable pipeline.
struct EnvironmentPrecomputeConstants
{
    uint Mode;
    uint Width;
    uint Height;
    uint SampleCount;
    float Roughness;
    float3 Padding;
};
HE_DECLARE_PUSH_CONSTANTS(
    EnvironmentPrecomputeConstants,
    g_EnvironmentPrecomputeConstants,
    space3);

#define g_Mode g_EnvironmentPrecomputeConstants.Mode
#define g_Width g_EnvironmentPrecomputeConstants.Width
#define g_Height g_EnvironmentPrecomputeConstants.Height
#define g_SampleCount g_EnvironmentPrecomputeConstants.SampleCount
#define g_Roughness g_EnvironmentPrecomputeConstants.Roughness

Texture2D<float4> g_Equirectangular : register(t0, space3);
TextureCube<float4> g_SourceCube : register(t1, space3);
RWTexture2DArray<float4> g_OutputCube : register(u0, space3);
RWTexture2D<float2> g_OutputBrdf : register(u1, space3);
SamplerState g_LinearSampler : register(s0, space3);

static const float c_Pi = 3.14159265358979323846f;
static const float c_MaxFiniteHalf = 65504.0f;

float3 SanitizeRadiance(float3 value)
{
    if (!all(isfinite(value)))
        return 0.0f;
    return clamp(value, 0.0f, c_MaxFiniteHalf);
}

float2 SanitizeBrdf(float2 value)
{
    return all(isfinite(value)) ? saturate(value) : 0.0f;
}

float RadicalInverseVdc(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

float2 Hammersley(uint index, uint count)
{
    return float2(float(index) / max(float(count), 1.0f), RadicalInverseVdc(index));
}

float3 FaceDirection(uint face, float2 uv)
{
    const float2 p = uv * 2.0f - 1.0f;
    if (face == 0u) return normalize(float3(1.0f, -p.y, -p.x));
    if (face == 1u) return normalize(float3(-1.0f, -p.y, p.x));
    if (face == 2u) return normalize(float3(p.x, 1.0f, p.y));
    if (face == 3u) return normalize(float3(p.x, -1.0f, -p.y));
    if (face == 4u) return normalize(float3(p.x, -p.y, 1.0f));
    return normalize(float3(-p.x, -p.y, -1.0f));
}

float3 SampleEquirectangular(float3 direction)
{
    const float2 uv = float2(
        atan2(direction.z, direction.x) * (0.5f / c_Pi) + 0.5f,
        -asin(clamp(direction.y, -1.0f, 1.0f)) / c_Pi + 0.5f);
    return SanitizeRadiance(
        g_Equirectangular.SampleLevel(g_LinearSampler, uv, 0.0f).rgb);
}

float3 BuildTangent(float3 normal)
{
    const float3 up = abs(normal.z) < 0.999f
        ? float3(0.0f, 0.0f, 1.0f)
        : float3(1.0f, 0.0f, 0.0f);
    return normalize(cross(up, normal));
}

float3 ImportanceSampleGGX(float2 xi, float3 normal, float roughness)
{
    const float alpha = max(roughness * roughness, 0.001f);
    const float phi = 2.0f * c_Pi * xi.x;
    const float cosTheta = sqrt((1.0f - xi.y) /
        max(1.0f + (alpha * alpha - 1.0f) * xi.y, 1e-5f));
    const float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    const float3 halfwayTangent = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    const float3 tangent = BuildTangent(normal);
    const float3 bitangent = cross(normal, tangent);
    return normalize(tangent * halfwayTangent.x + bitangent * halfwayTangent.y +
        normal * halfwayTangent.z);
}

float DistributionGGX(float nDotH, float roughness)
{
    const float alpha = max(roughness * roughness, 0.001f);
    const float alphaSquared = alpha * alpha;
    const float denominator = nDotH * nDotH * (alphaSquared - 1.0f) + 1.0f;
    return alphaSquared / max(c_Pi * denominator * denominator, 1e-12f);
}

#include "Shared/SurfaceLightingMath.hlsli"

float2 IntegrateBrdf(float nDotV, float roughness)
{
    const float3 view = float3(sqrt(saturate(1.0f - nDotV * nDotV)), 0.0f, nDotV);
    float scale = 0.0f;
    float bias = 0.0f;
    const uint count = max(g_SampleCount, 1u);
    for (uint sampleIndex = 0u; sampleIndex < count; ++sampleIndex)
    {
        const float3 halfway = ImportanceSampleGGX(
            Hammersley(sampleIndex, count), float3(0.0f, 0.0f, 1.0f), roughness);
        const float3 light = normalize(2.0f * dot(view, halfway) * halfway - view);
        const float nDotL = saturate(light.z);
        const float nDotH = saturate(halfway.z);
        const float vDotH = saturate(dot(view, halfway));
        if (nDotL <= 0.0f)
            continue;
        // The PDF cancels D and the view denominator, leaving 4*NoL*V*VoH/NoH.
        // Use exactly the same correlated visibility as direct illumination.
        const float visibility = 4.0f * nDotL *
            HE_VisibilitySmithGGXCorrelated(nDotV, nDotL, roughness) *
            vDotH / max(nDotH, 1e-5f);
        const float fresnel = pow(1.0f - vDotH, 5.0f);
        scale += (1.0f - fresnel) * visibility;
        bias += fresnel * visibility;
    }
    return float2(scale, bias) / float(count);
}

[numthreads(8, 8, 1)]
void CS(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g_Width || dispatchId.y >= g_Height)
        return;

    const float2 uv = (float2(dispatchId.xy) + 0.5f) / float2(g_Width, g_Height);
    if (g_Mode == 3u)
    {
        g_OutputBrdf[dispatchId.xy] = SanitizeBrdf(
            IntegrateBrdf(saturate(uv.x), saturate(uv.y)));
        return;
    }
    if (dispatchId.z >= 6u)
        return;

    const float3 normal = FaceDirection(dispatchId.z, uv);
    if (g_Mode == 0u)
    {
        g_OutputCube[dispatchId] = float4(SampleEquirectangular(normal), 1.0f);
        return;
    }
    if (g_Mode == 4u)
    {
        // Each source SRV exposes only the preceding cubemap mip. Sampling
        // child directions through TextureCube preserves the face convention
        // and hardware cubemap edge filtering throughout the low-pass chain.
        // +/- 0.25 destination texel lands on the two child texel centers in
        // the preceding mip, whose dimensions are exactly twice as large.
        const float2 quarterTexel = 0.25f / max(float2(g_Width, g_Height), 1.0f.xx);
        float3 filtered = 0.0f;
        filtered += SanitizeRadiance(g_SourceCube.SampleLevel(
            g_LinearSampler,
            FaceDirection(dispatchId.z, uv + float2(-quarterTexel.x, -quarterTexel.y)),
            0.0f).rgb);
        filtered += SanitizeRadiance(g_SourceCube.SampleLevel(
            g_LinearSampler,
            FaceDirection(dispatchId.z, uv + float2(quarterTexel.x, -quarterTexel.y)),
            0.0f).rgb);
        filtered += SanitizeRadiance(g_SourceCube.SampleLevel(
            g_LinearSampler,
            FaceDirection(dispatchId.z, uv + float2(-quarterTexel.x, quarterTexel.y)),
            0.0f).rgb);
        filtered += SanitizeRadiance(g_SourceCube.SampleLevel(
            g_LinearSampler,
            FaceDirection(dispatchId.z, uv + float2(quarterTexel.x, quarterTexel.y)),
            0.0f).rgb);
        g_OutputCube[dispatchId] = float4(
            SanitizeRadiance(filtered * 0.25f),
            1.0f);
        return;
    }

    const uint count = max(g_SampleCount, 1u);
    uint sourceWidth = 0u;
    uint sourceHeight = 0u;
    uint sourceMipCount = 0u;
    g_SourceCube.GetDimensions(0u, sourceWidth, sourceHeight, sourceMipCount);
    const float texelSolidAngle = 4.0f * c_Pi /
        max(6.0f * float(sourceWidth) * float(sourceHeight), 1.0f);
    const float sourceMaxMip = float(max(sourceMipCount, 1u) - 1u);
    float3 result = 0.0f;
    float weight = 0.0f;
    if (g_Mode == 1u)
    {
        // Integrate a fixed set of WORLD-space directions. Rotating a finite
        // hemisphere kernel with each output normal turns HDR quadrature error
        // into visible bands on smooth faces. Shared directions make neighboring
        // normals vary only through the continuous cosine weight.
        const uint diffuseCount = count * 4u;
        const float sampleSolidAngle = 4.0f * c_Pi / float(diffuseCount);
        const float sourceLod = clamp(
            0.5f * log2(max(sampleSolidAngle / texelSolidAngle, 1.0f)),
            0.0f, sourceMaxMip);
        for (uint sampleIndex = 0u; sampleIndex < diffuseCount; ++sampleIndex)
        {
            const float2 xi = Hammersley(sampleIndex, diffuseCount);
            const float z = 1.0f - 2.0f * (float(sampleIndex) + 0.5f) / float(diffuseCount);
            const float phi = 2.0f * c_Pi * xi.y;
            const float radial = sqrt(saturate(1.0f - z * z));
            const float3 sampleDirection = float3(radial * cos(phi), radial * sin(phi), z);
            const float cosineWeight = saturate(dot(normal, sampleDirection));
            result += SanitizeRadiance(g_SourceCube.SampleLevel(
                g_LinearSampler, sampleDirection, sourceLod).rgb) * cosineWeight;
        }
        result *= sampleSolidAngle;
    }
    else
    {
        const float3 view = normal;
        for (uint sampleIndex = 0u; sampleIndex < count; ++sampleIndex)
        {
            const float3 halfway = ImportanceSampleGGX(
                Hammersley(sampleIndex, count), normal, g_Roughness);
            const float3 light = normalize(2.0f * dot(view, halfway) * halfway - view);
            const float nDotL = saturate(dot(normal, light));
            if (nDotL <= 0.0f)
                continue;
            const float nDotH = saturate(dot(normal, halfway));
            const float hDotV = saturate(dot(halfway, view));
            const float distribution = DistributionGGX(nDotH, g_Roughness);
            const float pdf = max(distribution * nDotH / max(4.0f * hDotV, 1e-5f), 1e-6f);
            const float sampleSolidAngle = 1.0f / max(float(count) * pdf, 1e-6f);
            const float sourceLod = g_Roughness <= 1e-4f
                ? 0.0f
                : clamp(
                    0.5f * log2(max(sampleSolidAngle / texelSolidAngle, 1.0f)),
                    0.0f,
                    sourceMaxMip);
            result += SanitizeRadiance(g_SourceCube.SampleLevel(
                g_LinearSampler,
                light,
                sourceLod).rgb) * nDotL;
            weight += nDotL;
        }
        result /= max(weight, 1e-5f);
    }
    g_OutputCube[dispatchId] = float4(SanitizeRadiance(result), 1.0f);
}
