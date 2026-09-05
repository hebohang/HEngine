#pragma htype ps PSContract
#pragma htype lib

#include "Shared/BackendPushConstants.hlsli"

struct ContractFrameConstants
{
    float4x4 ViewProjection;
};

HE_DECLARE_PUSH_CONSTANTS(ContractFrameConstants, g_Frame, space0);
Texture2D<float4> g_MaterialTextures[1] : register(t0, space1);
RaytracingAccelerationStructure g_Scene : register(t0, space2);
StructuredBuffer<float4x4> g_InstanceTransforms : register(t1, space2);
RWTexture2D<float4> g_Output : register(u0, space3);
SamplerState g_MaterialSamplers[1] : register(s0, space4);

struct ContractPayload
{
    float3 Radiance;
};

float4 PSContract(float4 position : SV_Position, bool bFrontFace : SV_IsFrontFace) : SV_Target0
{
    const float4 projected = mul(g_Frame.ViewProjection, position);
    const float3 sampled = g_MaterialTextures[0].SampleLevel(
        g_MaterialSamplers[0], float2(0.5f, 0.5f), 0.0f).rgb;
    return float4(sampled + projected.xyz, bFrontFace ? 1.0f : 0.0f);
}

[shader("raygeneration")]
void RayGenerationContract()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    RayDesc ray;
    ray.Origin = float3(0.0f, 0.0f, 0.0f);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMin = 0.001f;
    ray.TMax = 1000.0f;

    ContractPayload payload = { float3(0.0f, 0.0f, 0.0f) };
    TraceRay(g_Scene, RAY_FLAG_NONE, 0xff, 0, 1, 0, ray, payload);
    g_Output[pixel] = float4(payload.Radiance, 1.0f);
}

[shader("miss")]
void MissContract(inout ContractPayload payload)
{
    payload.Radiance = float3(0.0f, 0.0f, 0.0f);
}

[shader("closesthit")]
void ClosestHitContract(
    inout ContractPayload payload,
    in BuiltInTriangleIntersectionAttributes attributes)
{
    const uint instanceId = InstanceID();
    const float4x4 instanceTransform = g_InstanceTransforms[instanceId];
    payload.Radiance = float3(attributes.barycentrics, instanceTransform[0][0]);
}

[shader("anyhit")]
void AnyHitContract(
    inout ContractPayload payload,
    in BuiltInTriangleIntersectionAttributes attributes)
{
    if (attributes.barycentrics.x < 0.0f)
        IgnoreHit();
}

[shader("callable")]
void CallableContract(inout ContractPayload payload)
{
    payload.Radiance *= 1.0f;
}
