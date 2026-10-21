#include "Modern/MaterialSurface.hlsli"
#pragma hmaterial Surface Lit
#pragma hblend Transparent
#pragma hcull Back
#pragma hdepthwrite Off
#pragma hshadow Off
#pragma property g_AlbedoMap "Sclera Color" texture2d default(white) group("Textures") id("pbr.albedo_map") semantic(BaseColor) colorspace(SRGB) fallback(white)
#pragma property g_NormalMap "Surface Normal" texture2d default(normal) group("Textures") id("pbr.normal_map") semantic(Normal) colorspace(Linear) fallback(normal)
#pragma property g_TransmissionMap "Cornea Transmission" texture2d default(white) group("Textures") id("cornea.transmission_map") semantic(Data) colorspace(Linear) fallback(white)
#pragma property g_BaseColorFactor "Sclera Tint" color default(1,1,1,1) group("Surface") id("pbr.base_color")
#pragma property g_RoughnessFactor "Roughness" float default(.08) range(0.045,1) group("Surface") id("pbr.roughness")
#pragma property g_NormalStrength "Normal Strength" float default(1) range(0,2) group("Surface") id("pbr.normal_strength")
#pragma property g_Ior "Index of Refraction" float default(1.376) range(1.01,2) group("Cornea") id("cornea.ior")

// Thin dielectric interface over separately authored opaque iris geometry.
// Transmission is composited without ray bending: this is a clear corneal
// coating, not volumetric or screen-space refraction. Black mask retains sclera.
void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface)
{
    const float3 sclera = g_AlbedoMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER, g_AlbedoMap_UV).rgb * g_BaseColorFactor.rgb;
    const float transmission = saturate(g_TransmissionMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER, g_TransmissionMap_UV).r);
    float variance;
    surface.NormalTS = HE_SAMPLE_NORMAL_MAP(g_NormalMap, HE_MATERIAL_DEFAULT_SAMPLER,
        g_NormalMap_UV, g_NormalStrength, variance);
    surface.NormalMipVariance = variance;
    surface.NormalUvSet = g_NormalMap_UV_SET;
    const float ior = clamp(g_Ior, 1.01f, 2.0f);
    const float f0 = pow((ior - 1.0f) / (ior + 1.0f), 2.0f);
    const float3 normalWS = HE_ResolveMaterialNormal(input, surface.NormalTS,
        surface.NormalUvSet);
    const float3 viewWS = HE_SafeNormalize(g_HECameraPositionAndExposure.xyz - input.PositionWS);
    const float fresnel = f0 + (1.0f - f0) * pow(1.0f - saturate(dot(normalWS, viewWS)), 5.0f);
    // Metallic=1 selects the existing specular-only GGX response with authored
    // dielectric F0; no metallic optical claim is made for the corneal tissue.
    surface.BaseColor = lerp(sclera, f0.xxx, transmission);
    surface.Metallic = transmission;
    surface.PerceptualRoughness = g_RoughnessFactor;
    surface.Emissive = 0;
    surface.Opacity = 1.0f - transmission * (1.0f - fresnel);
    surface.CharacterModel = 5u;
}
