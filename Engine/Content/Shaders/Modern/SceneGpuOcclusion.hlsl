#pragma htype cs CS

#include "Shared/BackendPushConstants.hlsli"

struct OcclusionConstants
{
    float4x4 ViewProjection;
    uint4 Counts; // draws, batches, base pyramid width/height
    uint4 Dimensions; // native width/height, mip count, operation
    float4 Parameters; // positive depth epsilon, base downsample
};
HE_DECLARE_PUSH_CONSTANTS(OcclusionConstants, g_Occlusion, space3);

struct GpuDrawData
{
    uint ObjectIndex;
    uint BatchIndex;
    uint VisibleBase;
    uint Padding;
};
struct DrawIndexedArguments
{
    uint IndexCount;
    uint InstanceCount;
    uint StartIndexLocation;
    int BaseVertexLocation;
    uint StartInstanceLocation;
};
StructuredBuffer<float4> g_Bounds : register(t0, space3);
StructuredBuffer<GpuDrawData> g_DrawData : register(t1, space3);
Texture2D<float> g_MaxDepthPyramid : register(t2, space3);
RWStructuredBuffer<uint> g_VisibleIndices : register(u0, space3);
RWStructuredBuffer<DrawIndexedArguments> g_Arguments : register(u1, space3);

bool FrustumVisible(float3 center, float radius)
{
    const float4 planes[6] = {
        g_Occlusion.ViewProjection[3] + g_Occlusion.ViewProjection[0],
        g_Occlusion.ViewProjection[3] - g_Occlusion.ViewProjection[0],
        g_Occlusion.ViewProjection[3] + g_Occlusion.ViewProjection[1],
        g_Occlusion.ViewProjection[3] - g_Occlusion.ViewProjection[1],
        g_Occlusion.ViewProjection[2],
        g_Occlusion.ViewProjection[3] - g_Occlusion.ViewProjection[2]
    };
    [unroll] for (uint plane = 0u; plane < 6u; ++plane)
        if (dot(planes[plane].xyz, center) + planes[plane].w < -radius * length(planes[plane].xyz))
            return false;
    return true;
}

bool FullyOccluded(float3 center, float radius)
{
    float2 minimumUv = float2(1.0, 1.0);
    float2 maximumUv = float2(0.0, 0.0);
    float nearestDepth = 1.0;
    // This box contains the complete world-space bounding sphere. With every
    // corner in front of the near plane, projection extrema occur at corners.
    // A near-plane crossing, non-finite matrix or unknown bound stays visible.
    [unroll] for (uint corner = 0u; corner < 8u; ++corner)
    {
        float3 signs = float3((corner & 1u) ? 1.0 : -1.0,
            (corner & 2u) ? 1.0 : -1.0, (corner & 4u) ? 1.0 : -1.0);
        float4 clip = mul(g_Occlusion.ViewProjection, float4(center + signs * radius, 1.0));
        if (!all(isfinite(clip)) || clip.w <= 1e-5 || clip.z <= 1e-5) return false;
        float3 ndc = clip.xyz / clip.w;
        float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
        minimumUv = min(minimumUv, uv);
        maximumUv = max(maximumUv, uv);
        nearestDepth = min(nearestDepth, ndc.z);
    }
    if (!isfinite(nearestDepth) || nearestDepth <= 0.0 || nearestDepth >= 1.0) return false;
    // Expand by a full native pixel for raster coverage/numerical conservatism.
    // Integer mapping follows the actual pyramid footprint: base blocks cover
    // four pixels; floor-sized mips merge their odd final row/column. Clamping
    // the shifted coordinate to each mip's last texel exactly covers that merge.
    int2 nativeMinimum = clamp(int2(floor(saturate(minimumUv) * float2(g_Occlusion.Dimensions.xy) - 1.0)),
        int2(0, 0), int2(g_Occlusion.Dimensions.xy) - 1);
    int2 nativeMaximum = clamp(int2(ceil(saturate(maximumUv) * float2(g_Occlusion.Dimensions.xy) + 1.0)),
        int2(0, 0), int2(g_Occlusion.Dimensions.xy) - 1);
    uint2 baseMinimum = uint2(nativeMinimum) / uint(g_Occlusion.Parameters.y);
    uint2 baseMaximum = uint2(nativeMaximum) / uint(g_Occlusion.Parameters.y);
    uint mip = 0u;
    uint2 minimum = baseMinimum;
    uint2 maximum = baseMaximum;
    [loop] while (mip + 1u < g_Occlusion.Dimensions.z && any(maximum - minimum + 1u > 4u))
    {
        ++mip;
        uint2 mipDimensions = max(g_Occlusion.Counts.zw >> mip, 1u);
        minimum = min(baseMinimum >> mip, mipDimensions - 1u);
        maximum = min(baseMaximum >> mip, mipDimensions - 1u);
    }
    float farthestOccluder = 0.0;
    [loop] for (uint y = minimum.y; y <= maximum.y; ++y)
    [loop] for (uint x = minimum.x; x <= maximum.x; ++x)
    {
        float depth = g_MaxDepthPyramid.Load(int3(uint2(x, y), mip));
        if (!isfinite(depth) || depth >= 1.0) return false;
        farthestOccluder = max(farthestOccluder, depth);
    }
    // MAX proves every pixel of the expanded bound is already covered by
    // nearer geometry. Equality/self-depth cannot reject the current object.
    return nearestDepth > farthestOccluder + g_Occlusion.Parameters.x;
}

[numthreads(64, 1, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (g_Occlusion.Dimensions.w == 0u)
    {
        if (index < g_Occlusion.Counts.y) g_Arguments[index].InstanceCount = 0u;
        return;
    }
    if (index >= g_Occlusion.Counts.x) return;
    GpuDrawData draw = g_DrawData[index];
    // CPU-certified main-view AABB rejection. Shadow inputs are independent.
    if ((draw.Padding & 1u) != 0u) return;
    float4 bounds = g_Bounds[draw.ObjectIndex];
    if (all(isfinite(bounds)) && bounds.w > 0.0)
    {
        if (!FrustumVisible(bounds.xyz, bounds.w) || FullyOccluded(bounds.xyz, bounds.w)) return;
    }
    uint compactIndex;
    InterlockedAdd(g_Arguments[draw.BatchIndex].InstanceCount, 1u, compactIndex);
    g_VisibleIndices[draw.VisibleBase + compactIndex] = draw.ObjectIndex;
}
