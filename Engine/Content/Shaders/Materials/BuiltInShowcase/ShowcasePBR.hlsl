#include "Modern/MaterialSurface.hlsli"

#pragma hmaterial Surface Lit
#pragma hblend Opaque
#pragma hcull Back
#pragma hdepthwrite On
#pragma hshadow On

#pragma property g_BaseColorFactor "Base Color" color default(0.8,0.8,0.8,1) group("Surface") id("pbr.base_color")
#pragma property g_MetallicFactor "Metallic" float default(0) range(0,1) group("Surface") id("pbr.metallic")
#pragma property g_RoughnessFactor "Roughness" float default(0.5) range(0.045,1) group("Surface") id("pbr.roughness")
#pragma property g_AoStrength "AO" float default(1) range(0,1) group("Surface") id("pbr.ao")
#pragma property g_EmissiveFactor "Emissive" color default(0,0,0,1) group("Emission") id("pbr.emissive")
#pragma property g_AlbedoMap "Base Color Map" texture2d default(white) group("Textures") id("pbr.albedo_map") semantic(BaseColor) colorspace(SRGB) fallback(white)
#pragma property g_NormalMap "Normal Map" texture2d default(normal) group("Textures") id("pbr.normal_map") semantic(Normal) colorspace(Linear) fallback(normal)
#pragma property g_NormalStrength "Normal Strength" float default(1) range(0,2) group("Normals") id("pbr.normal_strength")
#pragma property g_DetailNormalStrength "Procedural Detail" float default(0) range(0,1) group("Normals") id("pbr.detail_strength")
#pragma property g_DetailScale "Detail Scale" float default(12) range(1,64) group("Normals") id("pbr.detail_scale")

void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface)
{
    const float4 albedo = g_AlbedoMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER, input.UV0);
    const float3 sampledNormal = g_NormalMap.Sample(
        HE_MATERIAL_DEFAULT_SAMPLER, input.UV0).xyz * 2.0 - 1.0;
    const float2 detail = float2(
        sin(input.UV0.y * g_DetailScale * 6.2831853),
        cos(input.UV0.x * g_DetailScale * 6.2831853));
    surface.BaseColor = albedo.rgb * g_BaseColorFactor.rgb;
    surface.Metallic = g_MetallicFactor;
    surface.PerceptualRoughness = g_RoughnessFactor;
    surface.AO = g_AoStrength;
    surface.NormalTS = normalize(float3(
        sampledNormal.xy * g_NormalStrength + detail * g_DetailNormalStrength,
        max(sampledNormal.z, 0.1)));
    surface.Emissive = g_EmissiveFactor.rgb;
    surface.Opacity = albedo.a * g_BaseColorFactor.a;
}
