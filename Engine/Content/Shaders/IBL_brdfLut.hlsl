#include "Include/Common.hlsli"
#include "Include/PBR/BRDF.hlsli"
#include "Include/PBR/Sampling.hlsli"

#pragma htype vs VS
#pragma htype ps PS

struct BrdfLutVaryings
{
    float4 PositionHS : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

BrdfLutVaryings VS(VertexIn input)
{
    BrdfLutVaryings output;
    output.PositionHS = float4(input.pos.xy, 0.0f, 1.0f);
    output.TexCoord = float2(input.pos.x * 0.5f + 0.5f, 0.5f - input.pos.y * 0.5f);
    return output;
}

float2 IntegrateBrdf(float NoV, float roughness)
{
    const float3 normalWS = float3(0.0f, 0.0f, 1.0f);
    const float3 viewWS = float3(sqrt(max(1.0f - NoV * NoV, 0.0f)), 0.0f, NoV);
    const uint sampleCount = 1024u;

    float scale = 0.0f;
    float bias = 0.0f;
    [loop]
    for (uint i = 0u; i < sampleCount; ++i)
    {
        const float2 xi = Hammersley(i, sampleCount);
        const float3 halfWS = ImportanceSampleGGX(xi, normalWS, roughness);
        const float3 lightWS = SafeNormalize(2.0f * dot(viewWS, halfWS) * halfWS - viewWS);
        const float NoL = saturate(lightWS.z);
        const float NoH = saturate(halfWS.z);
        const float VoH = saturate(dot(viewWS, halfWS));
        if (NoL > 0.0f && NoH > 0.0f)
        {
            const float visibility = VisibilitySmithGGXCorrelated(NoV, NoL, roughness);
            const float visibilityWeight = 4.0f * NoL * visibility * VoH / max(NoH, 1e-4f);
            const float fresnel = Pow5(1.0f - VoH);
            scale += (1.0f - fresnel) * visibilityWeight;
            bias += fresnel * visibilityWeight;
        }
    }

    return float2(scale, bias) / float(sampleCount);
}

float4 PS(BrdfLutVaryings input) : SV_Target
{
    const float NoV = max(saturate(input.TexCoord.x), 1e-4f);
    const float roughness = clamp(input.TexCoord.y, 0.04f, 1.0f);
    return float4(IntegrateBrdf(NoV, roughness), 0.0f, 1.0f);
}
