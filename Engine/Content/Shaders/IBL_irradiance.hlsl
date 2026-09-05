#include "Include/Common.hlsli"
#include "Include/PBR/Sampling.hlsli"

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
    const uint sampleCount = 1024u;
    const float sourceResolution = max(g_SourceCubemapSize, 1.0f);
    const float sourceTexelSolidAngle = 4.0f * PI / (6.0f * sourceResolution * sourceResolution);

    float3 irradiance = float3(0.0f, 0.0f, 0.0f);
    [loop]
    for (uint i = 0u; i < sampleCount; ++i)
    {
        const float2 xi = Hammersley(i, sampleCount);
        const float3 sampleDir = ImportanceSampleCosineHemisphere(xi, normalWS);
        const float NoL = max(dot(normalWS, sampleDir), 1e-4f);
        const float pdf = NoL / PI;
        const float sampleSolidAngle = 1.0f / (float(sampleCount) * pdf);
        const float sourceMip = max(0.5f * log2(sampleSolidAngle / sourceTexelSolidAngle), 0.0f);
        irradiance += g_EnvironmentMap.SampleLevel(g_LinearClampSampler, sampleDir, sourceMip).rgb;
    }

    // Cosine-weighted sampling has pdf = NoL / PI, therefore the Monte Carlo
    // estimator of the irradiance integral is PI times the sample average.
    irradiance *= PI / float(sampleCount);
    return float4(irradiance, 1.0f);
}
