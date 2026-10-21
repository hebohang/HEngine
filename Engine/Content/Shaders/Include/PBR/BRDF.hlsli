#ifndef HENGINE_PBR_BRDF_HLSLI
#define HENGINE_PBR_BRDF_HLSLI

#include "Include/Common.hlsli"
#include "Shared/SurfaceLightingMath.hlsli"

struct PbrSurfaceData
{
    float3 BaseColor;
    float Metallic;
    float Roughness;
    float AmbientOcclusion;
    float3 Emissive;
    float Alpha;
};

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * Pow5(1.0f - saturate(cosTheta));
}

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    const float3 grazingF0 = max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), F0);
    return F0 + (grazingF0 - F0) * Pow5(1.0f - saturate(cosTheta));
}

float DistributionGGX(float NoH, float roughness)
{
    const float alpha = max(roughness * roughness, 0.0016f);
    const float alpha2 = alpha * alpha;
    const float denom = Square(NoH * NoH * (alpha2 - 1.0f) + 1.0f);
    return alpha2 / max(PI * denom, 1e-6f);
}

float VisibilitySmithGGXCorrelated(float NoV, float NoL, float roughness)
{
    return HE_VisibilitySmithGGXCorrelated(NoV, NoL, roughness);
}

float ComputeSpecularOcclusion(float NoV, float ao, float roughness)
{
    const float exponent = exp2(-16.0f * roughness - 1.0f);
    return saturate(pow(max(NoV + ao, 1e-4f), exponent) - 1.0f + ao);
}

float3 EvaluateDirectBRDF(
    PbrSurfaceData surface,
    float3 normalWS,
    float3 viewWS,
    float3 lightWS,
    float3 radiance,
    float3 F0)
{
    const float3 halfWS = SafeNormalize(viewWS + lightWS);
    const float NoV = saturate(dot(normalWS, viewWS));
    const float NoL = saturate(dot(normalWS, lightWS));
    const float NoH = saturate(dot(normalWS, halfWS));
    const float VoH = saturate(dot(viewWS, halfWS));
    if (NoV <= 0.0f || NoL <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);

    const float D = DistributionGGX(NoH, surface.Roughness);
    const float V = VisibilitySmithGGXCorrelated(NoV, NoL, surface.Roughness);
    const float3 F = FresnelSchlick(VoH, F0);

    const float3 kS = F;
    const float3 kD = (float3(1.0f, 1.0f, 1.0f) - kS) * (1.0f - surface.Metallic);
    const float3 diffuse = kD * surface.BaseColor / PI;
    const float3 specular = D * V * F;

    return (diffuse + specular) * radiance * NoL;
}

#endif
