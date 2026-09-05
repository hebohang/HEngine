#ifndef HENGINE_PBR_IBL_HLSLI
#define HENGINE_PBR_IBL_HLSLI

#include "BRDF.hlsli"

TextureCube g_IrradianceMap : register(t4, space3);
TextureCube g_PrefilterMap : register(t5, space3);
Texture2D g_BRDFLut : register(t6, space3);

cbuffer IBLData : register(b5, space3)
{
    float g_Exposure;
    float g_PrefilterMaxMip;
    float g_IBLIntensity;
    float _IBLPad;
}

float3 ToEnvironmentSampleDir(float3 directionWS)
{
    return SafeNormalize(directionWS);
}

float3 EvaluateImageBasedLighting(
    PbrSurfaceData surface,
    float3 normalWS,
    float3 geometricNormalWS,
    float3 viewWS,
    float3 F0)
{
    const float NoV = max(saturate(dot(normalWS, viewWS)), 1e-4f);
    const float3 reflectionWS = reflect(-viewWS, normalWS);
    // Low-frequency diffuse irradiance should not inherit every high-frequency
    // tangent-space normal variation. Blending toward the geometric normal
    // avoids dark environment lobes turning compressed normal-map detail into noise.
    const float3 diffuseNormalWS = SafeNormalize(lerp(geometricNormalWS, normalWS, 0.35f));
    const float3 iblNormalDir = ToEnvironmentSampleDir(diffuseNormalWS);
    const float3 iblReflectionDir = ToEnvironmentSampleDir(reflectionWS);
    const float3 F = FresnelSchlickRoughness(NoV, F0, surface.Roughness);
    const float3 kS = F;
    const float3 kD = (float3(1.0f, 1.0f, 1.0f) - kS) * (1.0f - surface.Metallic);

    const float3 irradiance = g_IrradianceMap.Sample(g_LinearClampSampler, iblNormalDir).rgb;
    const float3 diffuse = irradiance * surface.BaseColor;

    const float3 prefilteredColor = g_PrefilterMap.SampleLevel(
        g_LinearClampSampler,
        iblReflectionDir,
        surface.Roughness * g_PrefilterMaxMip).rgb;
    const float2 brdf = g_BRDFLut.Sample(g_LinearClampSampler, float2(NoV, surface.Roughness)).rg;
    // The split-sum LUT already integrates the view-dependent Fresnel term.
    // Feeding the roughness-adjusted Fresnel here applies Fresnel twice.
    const float3 specular = prefilteredColor * (F0 * brdf.x + brdf.y);
    const float specularOcclusion = ComputeSpecularOcclusion(NoV, surface.AmbientOcclusion, surface.Roughness);

    return ((kD * diffuse * surface.AmbientOcclusion) + specular * specularOcclusion) * g_IBLIntensity;
}

#endif
