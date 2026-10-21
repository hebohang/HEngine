#pragma htype cs CS

#include "Shared/BackendPushConstants.hlsli"

// GPU-driven opaque culling contract (space3 is pass-local):
// t0 objects, t1 world-space bounds, t2 object->batch records,
// u0 compact visible-object indices, u1 indexed indirect arguments.
struct GpuObjectData
{
    float4x4 World;
    float4x4 WorldInverseTranspose;
    uint4 MaterialObjectMeshIndices;
    float4x4 PreviousWorld;
    float4 StaticLightmapUvScaleBias;
    float4 StaticLightmapScale0;
    float4 StaticLightmapAdd0;
    float4 StaticLightmapScale1;
    float4 StaticLightmapAdd1;
    uint4 StaticLightmapDescriptor;
};

struct GpuBoundsData
{
    float4 CenterRadius;
};

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

struct CullConstants
{
    float4x4 ViewProjection;
    uint ObjectCount;
    uint3 Padding;
};
HE_DECLARE_PUSH_CONSTANTS(CullConstants, g_CullConstants, space3);

#define g_CullViewProjection g_CullConstants.ViewProjection
#define g_ObjectCount g_CullConstants.ObjectCount

StructuredBuffer<GpuObjectData> g_Objects : register(t0, space3);
StructuredBuffer<GpuBoundsData> g_Bounds : register(t1, space3);
StructuredBuffer<GpuDrawData> g_DrawData : register(t2, space3);
RWStructuredBuffer<uint> g_VisibleObjectIndices : register(u0, space3);
RWStructuredBuffer<DrawIndexedArguments> g_IndirectArguments : register(u1, space3);

bool IsSphereVisible(float3 center, float radius)
{
    // A world-space sphere projects to varying clip w. Expanding only clip xyz
    // loses that contribution and can reject visible geometry at frustum edges.
    // HLSL matrix indexing selects logical rows regardless of buffer packing.
    const float4 planes[6] = {
        g_CullViewProjection[3] + g_CullViewProjection[0],
        g_CullViewProjection[3] - g_CullViewProjection[0],
        g_CullViewProjection[3] + g_CullViewProjection[1],
        g_CullViewProjection[3] - g_CullViewProjection[1],
        g_CullViewProjection[2],
        g_CullViewProjection[3] - g_CullViewProjection[2]
    };
    if (!all(isfinite(center)) || !isfinite(radius) || radius <= 0.0f)
        return true;
    [unroll]
    for (uint planeIndex = 0; planeIndex < 6; ++planeIndex)
    {
        const float4 plane = planes[planeIndex];
        if (dot(plane.xyz, center) + plane.w < -radius * length(plane.xyz))
            return false;
    }
    return true;
}

[numthreads(64, 1, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint drawIndex = dispatchThreadId.x;
    if (drawIndex >= g_ObjectCount)
        return;

    const GpuDrawData draw = g_DrawData[drawIndex];
    // CPU-certified main-view AABB rejection. Shadow inputs are independent.
    if ((draw.Padding & 1u) != 0u) return;
    const GpuBoundsData bounds = g_Bounds[draw.ObjectIndex];
    if (!IsSphereVisible(bounds.CenterRadius.xyz, max(bounds.CenterRadius.w, 0.0f)))
        return;

    uint compactIndex = 0;
    InterlockedAdd(g_IndirectArguments[draw.BatchIndex].InstanceCount, 1, compactIndex);
    g_VisibleObjectIndices[draw.VisibleBase + compactIndex] = draw.ObjectIndex;
}
