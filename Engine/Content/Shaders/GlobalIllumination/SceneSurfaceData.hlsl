#pragma htype vs VS
#pragma htype ps PS

#include "Shared/BackendPushConstants.hlsli"

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

struct SurfaceFrameConstants
{
    float4x4 g_CurrentViewProjection;
    float4x4 g_PreviousViewProjection;
};
HE_DECLARE_PUSH_CONSTANTS(
    SurfaceFrameConstants,
    g_SurfaceFrameConstants,
    space0);

cbuffer SurfaceDrawConstants : register(b0, space2)
{
    uint g_StableInstanceId;
    uint3 g_SurfaceDrawPadding;
};

#define g_CurrentViewProjection \
    g_SurfaceFrameConstants.g_CurrentViewProjection
#define g_PreviousViewProjection \
    g_SurfaceFrameConstants.g_PreviousViewProjection

StructuredBuffer<RayTracingMaterialRecord> g_Materials : register(t4, space2);
StructuredBuffer<RayTracingInstanceRecord> g_Instances : register(t5, space2);

struct VertexInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
};

struct SurfaceVaryings
{
    float4 PositionCS : SV_Position;
    float4 CurrentClip : TEXCOORD0;
    float4 PreviousClip : TEXCOORD1;
    float3 NormalWS : TEXCOORD2;
    nointerpolation uint MaterialIndex : TEXCOORD3;
};

SurfaceVaryings VS(VertexInput input)
{
    // SV_InstanceID deliberately starts at zero for each DXIL draw and does not
    // include DrawArguments::startInstanceLocation. Use an explicit draw
    // constant so DXIL and SPIR-V select the same stable scene instance.
    const RayTracingInstanceRecord instance = g_Instances[g_StableInstanceId];
    const float4 position = float4(input.Position, 1.0f);
    const float4 positionWS = mul(instance.World, position);
    SurfaceVaryings output;
    output.PositionCS = mul(g_CurrentViewProjection, positionWS);
    output.CurrentClip = output.PositionCS;
    output.PreviousClip = mul(
        g_PreviousViewProjection,
        mul(instance.PreviousWorld, position));
    output.NormalWS = normalize(mul(
        (float3x3)instance.WorldInverseTranspose,
        input.Normal));
    output.MaterialIndex = instance.Indices.y;
    return output;
}

struct SurfaceOutputs
{
    float4 NormalRoughness : SV_Target0;
    float4 BaseColorMetalness : SV_Target1;
    float4 Emissive : SV_Target2;
    float2 MotionVectors : SV_Target3;
    uint MaterialId : SV_Target4;
    float PreviousDepth : SV_Target5;
    float2 ViewDepth : SV_Target6;
    uint2 SurfaceId : SV_Target7;
};

SurfaceOutputs PS(
    SurfaceVaryings input,
    bool bFrontFace : SV_IsFrontFace,
    uint primitiveId : SV_PrimitiveID)
{
    const RayTracingMaterialRecord material = g_Materials[input.MaterialIndex];
    const float faceSign = bFrontFace ? 1.0f : -1.0f;
    const float3 normal = normalize(input.NormalWS) * faceSign;
    const float inverseCurrentW = rcp(max(abs(input.CurrentClip.w), 1e-20f));
    const float inversePreviousW = rcp(max(abs(input.PreviousClip.w), 1e-20f));
    const float2 currentNdc = input.CurrentClip.xy * inverseCurrentW;
    const float2 previousNdc = input.PreviousClip.xy * inversePreviousW;
    // Current UV + motion vectors yields the previous-frame UV.
    const float2 motion = float2(
        (previousNdc.x - currentNdc.x) * 0.5f,
        (currentNdc.y - previousNdc.y) * 0.5f);

    SurfaceOutputs output;
    output.NormalRoughness = float4(
        normal,
        saturate(material.EmissiveRoughness.w));
    output.BaseColorMetalness = float4(
        max(material.BaseColorMetallic.rgb, 0.0f),
        saturate(material.BaseColorMetallic.w));
    output.Emissive = float4(max(material.EmissiveRoughness.rgb, 0.0f), 0.0f);
    output.MotionVectors = motion;
    output.MaterialId = input.MaterialIndex;
    output.PreviousDepth = input.PreviousClip.z * inversePreviousW;
    output.ViewDepth = float2(
        abs(input.CurrentClip.w),
        abs(input.PreviousClip.w));
    output.SurfaceId = uint2(g_StableInstanceId, primitiveId);
    return output;
}
