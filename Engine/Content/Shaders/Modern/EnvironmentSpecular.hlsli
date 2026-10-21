#ifndef HE_ENVIRONMENT_SPECULAR_HLSLI
#define HE_ENVIRONMENT_SPECULAR_HLSLI

float HE_ComputeSpecularOcclusion(float nDotV, float ao, float roughness)
{
    float exponent = exp2(-16.0 * roughness - 1.0);
    return saturate(pow(max(nDotV + ao, 1e-4), exponent) - 1.0 + ao);
}

// Shared by opaque lighting and the reflection replacement pass. The latter
// subtracts global IBL with this exact BRDF/occlusion convention before choosing
// screen-space or local-probe radiance, preventing duplicate indirect specular.
float3 HE_EnvironmentSpecularFactor(float3 baseColor, float metallic, float roughness,
    float nDotV, float ao, float2 brdf, float specularLevel = 0.5f)
{
    float3 f0 = lerp((0.08f * saturate(specularLevel)).xxx, baseColor, metallic);
    return (f0 * brdf.x + brdf.y) * HE_ComputeSpecularOcclusion(nDotV, ao, roughness);
}

#endif
