#pragma htype cs CS

#include "Shared/BackendPushConstants.hlsli"

struct ShadowCullConstants
{
    float4 FrustumPlanes[6];
    uint DrawCount;
    uint3 Padding;
};
HE_DECLARE_PUSH_CONSTANTS(ShadowCullConstants, g_ShadowCull, space3);

struct ShadowDraw
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
StructuredBuffer<float4> g_ShadowBounds : register(t0, space3);
StructuredBuffer<ShadowDraw> g_ShadowDraws : register(t1, space3);
RWStructuredBuffer<uint> g_ShadowVisible : register(u0, space3);
RWStructuredBuffer<DrawIndexedArguments> g_ShadowArguments : register(u1, space3);

[numthreads(64, 1, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_ShadowCull.DrawCount) return;
    const ShadowDraw draw = g_ShadowDraws[dispatchThreadId.x];
    const float4 bounds = g_ShadowBounds[draw.ObjectIndex];
    // Invalid/unavailable bounds remain visible. Parent retains animated and
    // material-deformed geometry on its conservative CPU fallback path.
    if (all(isfinite(bounds)) && bounds.w > 0.0f)
    {
        [unroll]
        for (uint plane = 0; plane < 6; ++plane)
        {
            const float4 equation = g_ShadowCull.FrustumPlanes[plane];
            if (dot(equation.xyz, bounds.xyz) + equation.w < -bounds.w) return;
        }
    }
    uint compactIndex;
    InterlockedAdd(g_ShadowArguments[draw.BatchIndex].InstanceCount, 1u, compactIndex);
    g_ShadowVisible[draw.VisibleBase + compactIndex] = draw.ObjectIndex;
}
