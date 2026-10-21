#include "Modern/MaterialSurface.hlsli"
#pragma hmaterial Surface Lit
#pragma hblend Opaque
#pragma hcull Back
#pragma hdepthwrite On
#pragma hshadow On
#pragma property g_AlbedoMap "Base Color / Coverage" texture2d default(white) group("Textures") id("pbr.albedo_map") semantic(BaseColor) colorspace(SRGB) fallback(white)
#pragma property g_NormalMap "Normal" texture2d default(normal) group("Textures") id("pbr.normal_map") semantic(Normal) colorspace(Linear) fallback(normal)
#pragma property g_IrisMaskMap "Iris Mask" texture2d default(black) group("Eye") id("eye.iris_mask") semantic(Data) colorspace(Linear) fallback(black)
#pragma property g_EmissiveMap "Emissive" texture2d default(black) group("Eye") id("eye.emissive_map") semantic(Emissive) colorspace(SRGB) fallback(black)
#pragma property g_GlossMap "Dielectric Specular" texture2d default(white) group("Eye") id("eye.gloss_map") semantic(Data) colorspace(Linear) fallback(white)
#pragma property g_UseGlossMap "Use Gloss Map" float default(0) range(0,1) group("Eye") id("eye.gloss_enabled")
#pragma property g_GlossIntensity "Gloss Intensity" float default(1) range(0,2) group("Eye") id("eye.gloss_intensity")
#pragma property g_UseIrisMask "Use Iris Mask" float default(0) range(0,1) group("Eye") id("eye.iris_mask_enabled")
#pragma property g_ScleraTint "Sclera Tint" color default(1,1,1,1) group("Eye") id("eye.sclera_tint")
#pragma property g_IrisTint "Iris Tint" color default(1,1,1,1) group("Eye") id("eye.iris_tint")
#pragma property g_EmissiveTint "Emissive Tint" color default(0,0,0,1) group("Eye") id("eye.emissive_tint")
#pragma property g_BaseColorFactor "Base Color" color default(1,1,1,1) group("Surface") id("pbr.base_color")
#pragma property g_RoughnessFactor "Roughness" float default(.09) range(0.045,1) group("Surface") id("pbr.roughness")
#pragma property g_NormalStrength "Normal Strength" float default(1) range(0,2) group("Surface") id("pbr.normal_strength")
// The asset must supply actual cornea/iris geometry and eyelid fit. This
// material provides a dielectric wet surface; it cannot create missing anatomy.
void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface)
{
    const float4 albedo = g_AlbedoMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER, g_AlbedoMap_UV);
    float variance;
    surface.NormalTS = HE_SAMPLE_NORMAL_MAP(g_NormalMap, HE_MATERIAL_DEFAULT_SAMPLER,
        g_NormalMap_UV, g_NormalStrength, variance);
    surface.NormalMipVariance = variance;
    surface.NormalUvSet = g_NormalMap_UV_SET;
    float3 eyeTint = float3(1.0, 1.0, 1.0);
    if (g_UseIrisMask > 0.5)
    {
        const float irisMask = g_IrisMaskMap.Sample(
            HE_MATERIAL_DEFAULT_SAMPLER, g_IrisMaskMap_UV).r;
        eyeTint = lerp(g_ScleraTint.rgb, g_IrisTint.rgb, saturate(irisMask));
    }
    surface.BaseColor = albedo.rgb * g_BaseColorFactor.rgb * eyeTint;
    surface.Metallic = 0;
    surface.SpecularLevel = g_UseGlossMap > 0.5f
        ? saturate(g_GlossMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER, g_GlossMap_UV).r * g_GlossIntensity)
        : 0.5f;
    surface.CharacterModel = 6u;
    surface.PerceptualRoughness = g_RoughnessFactor;
    surface.Emissive = g_EmissiveMap.Sample(
        HE_MATERIAL_DEFAULT_SAMPLER, g_EmissiveMap_UV).rgb * g_EmissiveTint.rgb;
    surface.Opacity = albedo.a * g_BaseColorFactor.a;
}
