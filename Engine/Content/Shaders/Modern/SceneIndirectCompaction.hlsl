#pragma htype cs CS

#include "Shared/BackendPushConstants.hlsli"

struct CompactionConstants { uint Candidates; uint Groups; uint CountByteOffset; uint Operation; };
HE_DECLARE_PUSH_CONSTANTS(CompactionConstants, g_Compaction, space3);
struct DrawIndexedArguments
{
    uint IndexCount;
    uint InstanceCount;
    uint StartIndexLocation;
    int BaseVertexLocation;
    uint StartInstanceLocation;
};
StructuredBuffer<DrawIndexedArguments> g_Input : register(t0, space3);
StructuredBuffer<uint2> g_GroupMapping : register(t1, space3);
RWByteAddressBuffer g_Compacted : register(u0, space3);

[numthreads(64, 1, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint candidate = dispatchThreadId.x;
    if (g_Compaction.Operation == 0u)
    {
        if (candidate < g_Compaction.Groups)
            g_Compacted.Store(g_Compaction.CountByteOffset + candidate * 4u, 0u);
        return;
    }
    if (candidate >= g_Compaction.Candidates) return;
    const DrawIndexedArguments args = g_Input[candidate];
    if (args.InstanceCount == 0u) return;
    const uint2 group = g_GroupMapping[candidate];
    uint compactIndex;
    g_Compacted.InterlockedAdd(g_Compaction.CountByteOffset + group.x * 4u, 1u, compactIndex);
    const uint destination = (group.y + compactIndex) * 20u;
    g_Compacted.Store4(destination, uint4(args.IndexCount, args.InstanceCount,
        args.StartIndexLocation, asuint(args.BaseVertexLocation)));
    g_Compacted.Store(destination + 16u, args.StartInstanceLocation);
}
