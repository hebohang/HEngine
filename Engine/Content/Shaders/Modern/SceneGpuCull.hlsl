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
    const float4 clip = mul(g_CullViewProjection, float4(center, 1.0f));
    const float3 rowLength = float3(
        length(float3(g_CullViewProjection[0][0], g_CullViewProjection[1][0], g_CullViewProjection[2][0])),
        length(float3(g_CullViewProjection[0][1], g_CullViewProjection[1][1], g_CullViewProjection[2][1])),
        length(float3(g_CullViewProjection[0][2], g_CullViewProjection[1][2], g_CullViewProjection[2][2])));
    const float radiusX = radius * max(rowLength.x, 1e-6f);
    const float radiusY = radius * max(rowLength.y, 1e-6f);
    const float radiusZ = radius * max(rowLength.z, 1e-6f);

    // D3D clip space: -w..w for x/y and 0..w for z.
    return clip.x >= -clip.w - radiusX && clip.x <= clip.w + radiusX &&
        clip.y >= -clip.w - radiusY && clip.y <= clip.w + radiusY &&
        clip.z >= -radiusZ && clip.z <= clip.w + radiusZ;
}

[numthreads(64, 1, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint drawIndex = dispatchThreadId.x;
    if (drawIndex >= g_ObjectCount)
        return;

    const GpuDrawData draw = g_DrawData[drawIndex];
    const GpuBoundsData bounds = g_Bounds[draw.ObjectIndex];
    if (!IsSphereVisible(bounds.CenterRadius.xyz, max(bounds.CenterRadius.w, 0.0f)))
        return;

    uint compactIndex = 0;
    InterlockedAdd(g_IndirectArguments[draw.BatchIndex].InstanceCount, 1, compactIndex);
    g_VisibleObjectIndices[draw.VisibleBase + compactIndex] = draw.ObjectIndex;
}
