#ifndef HENGINE_SKINNING_HLSLI
#define HENGINE_SKINNING_HLSLI

cbuffer BonesCBuffer : register(b4, space2)
{
    float4x4 g_Bones[100];
}

#define HENGINE_MAX_SKINNED_INSTANCES 31
#define HENGINE_MAX_BONES_PER_INSTANCE 32

cbuffer SkinnedInstancingCBuffer : register(b8, space2)
{
    float4x4 g_InstanceBones[HENGINE_MAX_SKINNED_INSTANCES * HENGINE_MAX_BONES_PER_INSTANCE];
    uint4 g_SkinnedInstancingSettings;
}

float4x4 ComputeSkinMatrix(int4 boneIds, float4 weights, uint instanceId)
{
    const float4 positiveWeights = max(weights, 0.0f);
    const float totalWeight = dot(positiveWeights, float4(1.0f, 1.0f, 1.0f, 1.0f));
    const float4 normalizedWeights = totalWeight > 1.0e-6f
        ? positiveWeights / totalWeight
        : float4(1.0f, 0.0f, 0.0f, 0.0f);

    if (g_SkinnedInstancingSettings.x != 0)
    {
        const uint boneBase = instanceId * HENGINE_MAX_BONES_PER_INSTANCE;
        return g_InstanceBones[boneBase + boneIds.x] * normalizedWeights.x +
               g_InstanceBones[boneBase + boneIds.y] * normalizedWeights.y +
               g_InstanceBones[boneBase + boneIds.z] * normalizedWeights.z +
               g_InstanceBones[boneBase + boneIds.w] * normalizedWeights.w;
    }

    return g_Bones[boneIds.x] * normalizedWeights.x +
           g_Bones[boneIds.y] * normalizedWeights.y +
           g_Bones[boneIds.z] * normalizedWeights.z +
           g_Bones[boneIds.w] * normalizedWeights.w;
}

#endif
