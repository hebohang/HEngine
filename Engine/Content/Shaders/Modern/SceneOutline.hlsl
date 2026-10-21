#pragma htype vs VS
#pragma htype ps PS
#pragma hvariant SKINNED

// Editor selection silhouette. It consumes only immutable RenderWorld data and
// runs in linear HDR before tone mapping.
cbuffer FrameConstants : register(b0, space0)
{
    float4x4 g_ViewProjection;
};

cbuffer ObjectConstants : register(b0, space2)
{
    float4x4 g_World;
    float4x4 g_WorldInverseTranspose;
    uint4 g_MaterialObjectMeshIndices;
};

#if SKINNED
StructuredBuffer<float4x4> g_BoneMatrices : register(t3, space2);
#endif

struct VertexInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
#if SKINNED
    int4 BoneIds[4] : BONEIDS;
    float4 BoneWeights[4] : BONEWEIGHTS;
#endif
};

float4 VS(VertexInput input) : SV_Position
{
    float4 positionLS = float4(input.Position, 1.0f);
    float3 normalLS = input.Normal;
#if SKINNED
    float4 skinnedPosition = 0.0f;
    float3 skinnedNormal = 0.0f;
    float totalWeight = 0.0f;
    [unroll]
    for (uint influence = 0; influence < 16; ++influence)
    {
        const int boneId = input.BoneIds[influence / 4][influence % 4];
        const float weight = input.BoneWeights[influence / 4][influence % 4];
        if (boneId < 0 || weight <= 0.0f)
            continue;
        const float4x4 bone =
            g_BoneMatrices[g_MaterialObjectMeshIndices.w + boneId];
        skinnedPosition += mul(bone, positionLS) * weight;
        skinnedNormal += mul((float3x3)bone, normalLS) * weight;
        totalWeight += weight;
    }
    if (totalWeight > 0.0f)
    {
        positionLS = skinnedPosition / totalWeight;
        normalLS = skinnedNormal / totalWeight;
    }
#endif
    float4 positionWS = mul(g_World, positionLS);
    const float3 normalWS = normalize(mul((float3x3)g_WorldInverseTranspose, normalLS));
    positionWS.xyz += normalWS * 0.025f;
    return mul(g_ViewProjection, positionWS);
}

float4 PS() : SV_Target0
{
    return float4(1.0f, 0.22f, 0.02f, 1.0f);
}
