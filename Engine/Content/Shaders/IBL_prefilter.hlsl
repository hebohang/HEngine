#include "Include/Common.hlsli"
#include "Include/PBR/Sampling.hlsli"
#include "Include/PBR/BRDF.hlsli"

#pragma htype vs VS
#pragma htype ps PS

TextureCube g_EnvironmentMap : register(t4, space3);

cbuffer IBLCapture : register(b3, space3)
{
    float4x4 g_CaptureViewProjection;
    float g_Roughness;
    uint g_CaptureFaceIndex;
    float2 g_CaptureInvSize;
    float g_SourceCubemapSize;
    float3 _IBLCapturePadding;
}

struct CubeCaptureVaryings
{
    float4 PositionHS : SV_POSITION;
};

CubeCaptureVaryings VS(VertexIn input)
{
    CubeCaptureVaryings output;
    output.PositionHS = mul(g_CaptureViewProjection, float4(input.pos, 1.0f));
    return output;
}

float4 PS(CubeCaptureVaryings input) : SV_Target
{
    const float3 normalWS = CubeCaptureDirection(g_CaptureFaceIndex, input.PositionHS, g_CaptureInvSize);
    const float3 viewWS = normalWS;
    if (g_Roughness <= 0.001f)
    {
        return float4(g_EnvironmentMap.SampleLevel(g_LinearClampSampler, normalWS, 0.0f).rgb, 1.0f);
    }

    const uint sampleCount = 1024u;
    const float sourceResolution = max(g_SourceCubemapSize, 1.0f);
    const float saTexel = 4.0f * PI / (6.0f * sourceResolution * sourceResolution);

    float3 prefilteredColor = float3(0.0f, 0.0f, 0.0f);
    float totalWeight = 0.0f;

    [loop]
    for (uint i = 0u; i < sampleCount; ++i)
    {
        const float2 xi = Hammersley(i, sampleCount);
        const float3 halfWS = ImportanceSampleGGX(xi, normalWS, g_Roughness);
        const float3 lightWS = SafeNormalize(2.0f * dot(viewWS, halfWS) * halfWS - viewWS);
        const float NoL = saturate(dot(normalWS, lightWS));
        if (NoL > 0.0f)
        {
            const float NoH = saturate(dot(normalWS, halfWS));
            const float VoH = saturate(dot(viewWS, halfWS));
            const float D = DistributionGGX(NoH, g_Roughness);
            const float pdf = max(D * NoH / max(4.0f * VoH, 1e-4f), 1e-4f);
            const float saSample = 1.0f / (float(sampleCount) * pdf);
            const float mipLevel = g_Roughness <= 0.001f ? 0.0f : max(0.5f * log2(saSample / saTexel), 0.0f);

            prefilteredColor += g_EnvironmentMap.SampleLevel(g_LinearClampSampler, lightWS, mipLevel).rgb * NoL;
            totalWeight += NoL;
        }
    }

    prefilteredColor /= max(totalWeight, 1e-4f);
    return float4(prefilteredColor, 1.0f);
}
