#pragma htype cs CS

#include "RasterSurfaceData.hlsli"
#include "EnvironmentSpecular.hlsli"

struct ProbeConstants
{
    float4 BoundsMinBlend;
    float4 BoundsMaxIntensity;
    float4 CapturePositionMaximumMip;
};
cbuffer ReflectionFrame : register(b0, space3)
{
    float4x4 g_InverseViewProjection;
    float4x4 g_ViewProjection;
    float4 g_CameraPositionIblIntensity;
    float4 g_ViewForwardGlobalMaximumMip;
    uint4 g_Dimensions;
    float4 g_TraceParameters; // maximum distance, thickness, roughness cutoff, edge fade
    uint4 g_TraceOptions; // steps, binary steps, SSR enabled, probe count
    float4 g_NormalBiasOperation;
    ProbeConstants g_Probes[8];
    uint4 g_DiffuseProbeMask;
};
Texture2D<float4> g_OpaqueHdr : register(t0, space3);
Texture2D<float> g_Depth : register(t1, space3);
Texture2D<float4> g_BaseColorMetallic : register(t2, space3);
Texture2D<float4> g_NormalRoughnessAo : register(t3, space3);
Texture2D<float4> g_EmissiveModel : register(t4, space3);
Texture2D<float> g_Ao : register(t5, space3);
TextureCube<float4> g_GlobalPrefilter : register(t6, space3);
Texture2D<float2> g_BrdfLut : register(t7, space3);
TextureCube<float4> g_Probe0 : register(t8, space3);
TextureCube<float4> g_Probe1 : register(t9, space3);
TextureCube<float4> g_Probe2 : register(t10, space3);
TextureCube<float4> g_Probe3 : register(t11, space3);
TextureCube<float4> g_Probe4 : register(t12, space3);
TextureCube<float4> g_Probe5 : register(t13, space3);
TextureCube<float4> g_Probe6 : register(t14, space3);
TextureCube<float4> g_Probe7 : register(t15, space3);
Texture2D<float4> g_HalfSsr : register(t16, space3);
TextureCube<float4> g_GlobalIrradiance : register(t17, space3);
TextureCube<float4> g_ProbeIrradiance0 : register(t18, space3);
TextureCube<float4> g_ProbeIrradiance1 : register(t19, space3);
TextureCube<float4> g_ProbeIrradiance2 : register(t20, space3);
TextureCube<float4> g_ProbeIrradiance3 : register(t21, space3);
TextureCube<float4> g_ProbeIrradiance4 : register(t22, space3);
TextureCube<float4> g_ProbeIrradiance5 : register(t23, space3);
TextureCube<float4> g_ProbeIrradiance6 : register(t24, space3);
TextureCube<float4> g_ProbeIrradiance7 : register(t25, space3);
SamplerState g_LinearClamp : register(s0, space3);
RWTexture2D<float4> g_Output : register(u0, space3);

groupshared float3 s_SsrGuidePosition[36];
groupshared float3 s_SsrGuideNormal[36];
groupshared float4 s_SsrGuideRadiance[36];
groupshared uint s_SsrGuideValid[36];

float3 PositionAt(int2 pixel, float depth)
{
    return HE_ReconstructRasterPosition((float2(pixel) + 0.5) / float2(g_Dimensions.xy),
        depth, g_InverseViewProjection);
}
float ViewDepth(float3 position)
{
    return dot(position - g_CameraPositionIblIntensity.xyz, g_ViewForwardGlobalMaximumMip.xyz);
}
bool IsSurface(int2 pixel)
{
    return g_Depth.Load(int3(pixel, 0)) < 0.999999 && g_EmissiveModel.Load(int3(pixel, 0)).a < 0.5;
}
// A nearest-covered representative preserves thin foreground silhouettes. All
// guided reconstruction uses this same representative, including odd borders.
int2 Representative(int2 halfPixel)
{
    int2 origin = clamp(halfPixel * 2, 0, int2(g_Dimensions.xy) - 1);
    int2 best = origin;
    float bestDepth = 1.0;
    [unroll] for (int y = 0; y < 2; ++y)
    [unroll] for (int x = 0; x < 2; ++x)
    {
        int2 samplePixel = min(origin + int2(x, y), int2(g_Dimensions.xy) - 1);
        float sampleDepth = g_Depth.Load(int3(samplePixel, 0));
        if (sampleDepth < bestDepth) { best = samplePixel; bestDepth = sampleDepth; }
    }
    return best;
}
bool Project(float3 position, out float2 uv)
{
    float4 clip = mul(g_ViewProjection, float4(position, 1.0));
    uv = clip.xy / max(clip.w, 1e-6) * float2(0.5, -0.5) + 0.5;
    return clip.w > 1e-6 && clip.z > 0.0 && clip.z < clip.w &&
        all(uv > 0.0) && all(uv < 1.0);
}
float SceneDepthAt(float2 uv)
{
    int2 pixel = clamp(int2(uv * float2(g_Dimensions.xy)), 0, int2(g_Dimensions.xy) - 1);
    float depth = g_Depth.Load(int3(pixel, 0));
    return depth < 0.999999 ? ViewDepth(PositionAt(pixel, depth)) : 1e20;
}
float3 HitRadiance(float2 uv, float roughness, float sceneDepth)
{
    // Small depth-guided source footprint suppresses a perfectly sharp mirror
    // response on rough surfaces. This is a bounded spatial approximation to
    // GGX convolution; surfaces above the cutoff use prefiltered probes instead.
    float radius = min(8.0, 16.0 * roughness * roughness);
    float3 radiance = g_OpaqueHdr.SampleLevel(g_LinearClamp, uv, 0).rgb;
    float weightSum = 1.0;
    const float2 offsets[4] = { float2(-1, 0), float2(1, 0), float2(0, -1), float2(0, 1) };
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        float2 sampleUv = uv + offsets[i] * radius / float2(g_Dimensions.xy);
        if (any(sampleUv <= 0.0) || any(sampleUv >= 1.0)) continue;
        float sampleDepth = SceneDepthAt(sampleUv);
        float weight = exp2(-abs(sampleDepth - sceneDepth) / max(g_TraceParameters.y, 0.01));
        radiance += g_OpaqueHdr.SampleLevel(g_LinearClamp, sampleUv, 0).rgb * weight;
        weightSum += weight;
    }
    return radiance / weightSum;
}
float4 Trace(int2 pixel)
{
    if (!IsSurface(pixel)) return 0.0;
    float4 packed = g_NormalRoughnessAo.Load(int3(pixel, 0));
    float roughness = packed.z;
    if (roughness >= g_TraceParameters.z) return 0.0;
    float3 normal = HE_DecodeOctahedralNormal(packed.xy);
    float3 position = PositionAt(pixel, g_Depth.Load(int3(pixel, 0)));
    float3 view = normalize(g_CameraPositionIblIntensity.xyz - position);
    float3 ray = reflect(-view, normal);
    if (dot(view, normal) <= 0.0) return 0.0;
    float3 origin = position + normal * g_NormalBiasOperation.x;
    float previousDistance = 0.0;
    float previousDelta = -g_TraceParameters.y;
    [loop] for (uint step = 1; step <= g_TraceOptions.x; ++step)
    {
        float fraction = float(step) / float(g_TraceOptions.x);
        float distance = g_TraceParameters.x * fraction * fraction;
        float3 rayPosition = origin + ray * distance;
        float2 uv;
        if (!Project(rayPosition, uv)) return 0.0;
        float sceneDepth = SceneDepthAt(uv);
        float delta = ViewDepth(rayPosition) - sceneDepth;
        if (delta >= 0.0 && previousDelta <= 0.0)
        {
            float lower = previousDistance;
            float upper = distance;
            [loop] for (uint refinement = 0; refinement < g_TraceOptions.y; ++refinement)
            {
                float middle = 0.5 * (lower + upper);
                float3 middlePosition = origin + ray * middle;
                float2 middleUv;
                if (!Project(middlePosition, middleUv)) { upper = middle; continue; }
                float middleDelta = ViewDepth(middlePosition) - SceneDepthAt(middleUv);
                if (middleDelta >= 0.0) upper = middle;
                else lower = middle;
            }
            float3 hit = origin + ray * upper;
            if (!Project(hit, uv)) return 0.0;
            sceneDepth = SceneDepthAt(uv);
            float hitDelta = ViewDepth(hit) - sceneDepth;
            int2 hitPixel = clamp(int2(uv * float2(g_Dimensions.xy)), 0, int2(g_Dimensions.xy) - 1);
            float3 hitNormal = HE_DecodeOctahedralNormal(g_NormalRoughnessAo.Load(int3(hitPixel, 0)).xy);
            // Frontmost-depth SSR cannot see hidden geometry or back faces.
            // Reject ambiguous intersections and use the probe/environment.
            if (hitDelta >= 0.0 && hitDelta <= g_TraceParameters.y &&
                upper > max(g_NormalBiasOperation.x * 2.0, 0.04) &&
                dot(hitNormal, ray) < -0.03 && any(abs(hitPixel - pixel) > 1))
            {
                float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
                float confidence = saturate(edge / g_TraceParameters.w);
                confidence *= saturate(1.0 - upper / g_TraceParameters.x);
                confidence *= 1.0 - smoothstep(g_TraceParameters.z * 0.75, g_TraceParameters.z, roughness);
                return float4(clamp(HitRadiance(uv, roughness, sceneDepth), 0.0, 65504.0), confidence);
            }
        }
        previousDistance = distance;
        previousDelta = delta;
    }
    return 0.0;
}

void LoadSsrGuides(uint2 groupId, uint groupIndex)
{
    if (groupIndex >= 36u) return;
    const int2 halfPixel = int2(groupId * 4u) - 1 + int2(groupIndex % 6u, groupIndex / 6u);
    s_SsrGuideValid[groupIndex] = 0u;
    if (any(halfPixel < 0) || any(halfPixel >= int2(g_Dimensions.zw))) return;
    const int2 source = Representative(halfPixel);
    if (!IsSurface(source)) return;
    s_SsrGuidePosition[groupIndex] = PositionAt(source, g_Depth.Load(int3(source, 0)));
    s_SsrGuideNormal[groupIndex] = HE_DecodeOctahedralNormal(g_NormalRoughnessAo.Load(int3(source, 0)).xy);
    s_SsrGuideRadiance[groupIndex] = g_HalfSsr.Load(int3(halfPixel, 0));
    s_SsrGuideValid[groupIndex] = 1u;
}

float4 GuidedSsr(int2 pixel, float3 position, float3 normal)
{
    float2 halfCoordinate = (float2(pixel) + 0.5) * 0.5 - 0.5;
    int2 center = int2(round(halfCoordinate));
    float4 sum = 0.0;
    float totalWeight = 0.0;
    float depthTolerance = max(0.025, ViewDepth(position) * 0.002);
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int2 halfPixel = center + int2(x, y);
        if (any(halfPixel < 0) || any(halfPixel >= int2(g_Dimensions.zw))) continue;
        float3 samplePosition, sampleNormal;
        float4 ssrSample;
        if (g_NormalBiasOperation.z > 0.5)
        {
            const int2 tileOrigin = (pixel / 8) * 4 - 1;
            const int2 local = halfPixel - tileOrigin;
            const uint guideIndex = uint(local.y * 6 + local.x);
            if (s_SsrGuideValid[guideIndex] == 0u) continue;
            samplePosition = s_SsrGuidePosition[guideIndex];
            sampleNormal = s_SsrGuideNormal[guideIndex];
            ssrSample = s_SsrGuideRadiance[guideIndex];
        }
        else
        {
            int2 source = Representative(halfPixel);
            if (!IsSurface(source)) continue;
            samplePosition = PositionAt(source, g_Depth.Load(int3(source, 0)));
            sampleNormal = HE_DecodeOctahedralNormal(g_NormalRoughnessAo.Load(int3(source, 0)).xy);
            ssrSample = g_HalfSsr.Load(int3(halfPixel, 0));
        }
        float planeDistance = abs(dot(samplePosition - position, normal));
        float depthDistance = abs(ViewDepth(samplePosition) - ViewDepth(position));
        float weight = pow(saturate(dot(normal, sampleNormal)), 32.0);
        weight *= exp2(-planeDistance / depthTolerance - depthDistance / (depthTolerance * 4.0));
        float2 offset = float2(halfPixel) - halfCoordinate;
        weight *= exp2(-dot(offset, offset));
        sum.rgb += ssrSample.rgb * ssrSample.a * weight;
        sum.a += ssrSample.a * weight;
        totalWeight += weight;
    }
    return float4(sum.rgb / max(sum.a, 1e-6), sum.a / max(totalWeight, 1e-6));
}

float3 SampleProbe(uint index, float3 direction, float mip)
{
    // Explicit texture selection works on the descriptor-page/Vulkan baseline;
    // it does not require dynamically indexed bindless cube descriptors.
    switch (index)
    {
    case 0: return g_Probe0.SampleLevel(g_LinearClamp, direction, mip).rgb;
    case 1: return g_Probe1.SampleLevel(g_LinearClamp, direction, mip).rgb;
    case 2: return g_Probe2.SampleLevel(g_LinearClamp, direction, mip).rgb;
    case 3: return g_Probe3.SampleLevel(g_LinearClamp, direction, mip).rgb;
    case 4: return g_Probe4.SampleLevel(g_LinearClamp, direction, mip).rgb;
    case 5: return g_Probe5.SampleLevel(g_LinearClamp, direction, mip).rgb;
    case 6: return g_Probe6.SampleLevel(g_LinearClamp, direction, mip).rgb;
    default: return g_Probe7.SampleLevel(g_LinearClamp, direction, mip).rgb;
    }
}
float3 ProbeRadiance(float3 position, float3 reflection, float roughness, float3 globalRadiance)
{
    float3 sum = 0.0;
    float weightSum = 0.0;
    [loop] for (uint i = 0; i < g_TraceOptions.w; ++i)
    {
        ProbeConstants probe = g_Probes[i];
        float3 nearBoundary = position - probe.BoundsMinBlend.xyz;
        float3 farBoundary = probe.BoundsMaxIntensity.xyz - position;
        float3 boundaryDistance = min(nearBoundary, farBoundary);
        float distance = min(boundaryDistance.x, min(boundaryDistance.y, boundaryDistance.z));
        if (distance <= 0.0) continue;
        float weight = smoothstep(0.0, probe.BoundsMinBlend.w, distance);
        float3 safeRay = float3(reflection.x >= 0.0 ? 1.0 : -1.0,
            reflection.y >= 0.0 ? 1.0 : -1.0, reflection.z >= 0.0 ? 1.0 : -1.0) * max(abs(reflection), 1e-6);
        float3 exitDistance = max((probe.BoundsMinBlend.xyz - position) / safeRay,
            (probe.BoundsMaxIntensity.xyz - position) / safeRay);
        float boxExit = min(exitDistance.x, min(exitDistance.y, exitDistance.z));
        float3 corrected = position + reflection * max(boxExit, 0.0) - probe.CapturePositionMaximumMip.xyz;
        if (dot(corrected, corrected) < 1e-10) corrected = reflection;
        sum += SampleProbe(i, corrected, roughness * probe.CapturePositionMaximumMip.w) *
            probe.BoundsMaxIntensity.w * weight;
        weightSum += weight;
    }
    if (weightSum <= 1e-6) return globalRadiance;
    // Total local influence approaches zero at volume boundaries, avoiding a
    // discontinuous switch from an arbitrarily small local weight to global IBL.
    return lerp(globalRadiance, sum / weightSum, saturate(weightSum));
}
float3 SampleProbeIrradiance(uint index, float3 normal)
{
    switch (index)
    {
    case 0: return g_ProbeIrradiance0.SampleLevel(g_LinearClamp, normal, 0).rgb;
    case 1: return g_ProbeIrradiance1.SampleLevel(g_LinearClamp, normal, 0).rgb;
    case 2: return g_ProbeIrradiance2.SampleLevel(g_LinearClamp, normal, 0).rgb;
    case 3: return g_ProbeIrradiance3.SampleLevel(g_LinearClamp, normal, 0).rgb;
    case 4: return g_ProbeIrradiance4.SampleLevel(g_LinearClamp, normal, 0).rgb;
    case 5: return g_ProbeIrradiance5.SampleLevel(g_LinearClamp, normal, 0).rgb;
    case 6: return g_ProbeIrradiance6.SampleLevel(g_LinearClamp, normal, 0).rgb;
    default: return g_ProbeIrradiance7.SampleLevel(g_LinearClamp, normal, 0).rgb;
    }
}
float3 ProbeIrradiance(float3 position, float3 normal, float3 globalIrradiance)
{
    float3 sum = 0.0f;
    float weightSum = 0.0f;
    [loop] for (uint i = 0; i < g_TraceOptions.w; ++i)
    {
        if ((g_DiffuseProbeMask.x & (1u << i)) == 0u) continue;
        ProbeConstants probe = g_Probes[i];
        const float3 boundary = min(position - probe.BoundsMinBlend.xyz,
            probe.BoundsMaxIntensity.xyz - position);
        const float distance = min(boundary.x, min(boundary.y, boundary.z));
        if (distance <= 0.0f) continue;
        const float weight = smoothstep(0.0f, probe.BoundsMinBlend.w, distance);
        // Diffuse uses the actual cosine-convolved irradiance, NOT the roughest
        // GGX prefilter mip and NOT parallax projection of a reflection ray.
        sum += SampleProbeIrradiance(i, normal) * probe.BoundsMaxIntensity.w * weight;
        weightSum += weight;
    }
    return weightSum > 1e-6f
        ? lerp(globalIrradiance, sum / weightSum, saturate(weightSum)) : globalIrradiance;
}
float4 Composite(int2 pixel)
{
    float4 source = g_OpaqueHdr.Load(int3(pixel, 0));
    if (!IsSurface(pixel)) return source;
    float4 packed = g_NormalRoughnessAo.Load(int3(pixel, 0));
    float4 material = g_BaseColorMetallic.Load(int3(pixel, 0));
    const float modelCode = -g_EmissiveModel.Load(int3(pixel, 0)).a;
    const float specularLevel = (modelCode >= 2.0f && modelCode < 3.0f) ||
        (modelCode >= 6.0f && modelCode < 7.0f)
        ? saturate(frac(modelCode) * 2.0f) : 0.5f;
    float3 position = PositionAt(pixel, g_Depth.Load(int3(pixel, 0)));
    float3 normal = HE_DecodeOctahedralNormal(packed.xy);
    float3 view = normalize(g_CameraPositionIblIntensity.xyz - position);
    float3 reflection = reflect(-view, normal);
    float nDotV = saturate(dot(normal, view));
    float3 globalRadiance = g_GlobalPrefilter.SampleLevel(g_LinearClamp, reflection,
        packed.z * g_ViewForwardGlobalMaximumMip.w).rgb * g_CameraPositionIblIntensity.w;
    float3 chosenRadiance = ProbeRadiance(position, reflection, packed.z, globalRadiance);
    if (g_TraceOptions.z != 0u && packed.z < g_TraceParameters.z)
    {
        float4 ssr = GuidedSsr(pixel, position, normal);
        chosenRadiance = lerp(chosenRadiance, ssr.rgb, saturate(ssr.a));
    }
    uint aoWidth, aoHeight;
    g_Ao.GetDimensions(aoWidth, aoHeight);
    int2 aoPixel = min(int2((float2(pixel) + 0.5) / float2(g_Dimensions.xy) * float2(aoWidth, aoHeight)),
        int2(aoWidth, aoHeight) - 1);
    float ao = packed.w * g_Ao.Load(int3(aoPixel, 0));
    float2 brdf = g_BrdfLut.SampleLevel(g_LinearClamp, float2(nDotV, packed.z), 0);
    float3 factor = HE_EnvironmentSpecularFactor(material.rgb, material.a, packed.z, nDotV, ao, brdf,
        specularLevel);
    float3 result = source.rgb + (chosenRadiance - globalRadiance) * factor;
    if (g_DiffuseProbeMask.x != 0u && g_NormalBiasOperation.w > 0.0f)
    {
        const float3 globalDiffuse = g_GlobalIrradiance.SampleLevel(g_LinearClamp, normal, 0).rgb *
            g_CameraPositionIblIntensity.w;
        const float3 localDiffuse = ProbeIrradiance(position, normal, globalDiffuse);
        const float3 f0 = lerp((0.08f * specularLevel).xxx, material.rgb, material.a);
        const float3 fresnel = f0 + (max((1.0f - packed.z).xxx, f0) - f0) * pow(1.0f - nDotV, 5.0f);
        const float3 diffuseFactor = (1.0f - fresnel) * (1.0f - material.a) *
            material.rgb / 3.14159265f * ao * g_NormalBiasOperation.w;
        result += (localDiffuse - globalDiffuse) * diffuseFactor;
    }
    return float4(all(isfinite(result)) ? clamp(result, 0.0, 65504.0) : source.rgb, source.a);
}

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    uint2 pixel = dispatchThreadId.xy;
    if (g_NormalBiasOperation.y < 0.5)
    {
        if (any(pixel >= g_Dimensions.zw)) return;
        g_Output[pixel] = Trace(Representative(int2(pixel)));
    }
    else
    {
        if (g_TraceOptions.z != 0u && g_NormalBiasOperation.z > 0.5)
        {
            LoadSsrGuides(groupId.xy, groupIndex);
            GroupMemoryBarrierWithGroupSync();
        }
        if (any(pixel >= g_Dimensions.xy)) return;
        g_Output[pixel] = Composite(int2(pixel));
    }
}
