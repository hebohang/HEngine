#include "Modern/MaterialSurface.hlsli"
#pragma hmaterial Surface Lit
#pragma hblend Opaque
#pragma hcull None
#pragma hdepthwrite On
#pragma hshadow On
#pragma property g_AlbedoMap "Base Color / Coverage" texture2d default(white) group("Textures") id("pbr.albedo_map") semantic(BaseColor) colorspace(SRGB) fallback(white)
#pragma property g_NormalMap "Normal" texture2d default(normal) group("Textures") id("pbr.normal_map") semantic(Normal) colorspace(Linear) fallback(normal)
#pragma property g_ClothSurfaceMap "Metallic (Blue) / Roughness (Green)" texture2d default(white) group("Textures") id("cloth.surface_map") semantic(MetallicRoughness) colorspace(Linear) fallback(white)
#pragma property g_ClothEmissiveMap "Embroidery Emission" texture2d default(white) group("Textures") id("cloth.emissive_map") semantic(Emissive) colorspace(SRGB) fallback(white)
#pragma property g_MaterialIdMap "Material ID" texture2d default(black) group("Palette") id("cloth.material_id") semantic(Data) colorspace(Linear) fallback(black)
#pragma property g_DirtMaskMap "Dirt Mask" texture2d default(black) group("Weathering") id("cloth.dirt_mask") semantic(Data) colorspace(Linear) fallback(black)
#pragma property g_BloodMaskMap "Blood Mask" texture2d default(black) group("Weathering") id("cloth.blood_mask") semantic(Data) colorspace(Linear) fallback(black)
#pragma property g_UseDirt "Use Dirt" float default(0) range(0,1) group("Weathering") id("cloth.dirt_enabled")
#pragma property g_UseBlood "Use Blood" float default(0) range(0,1) group("Weathering") id("cloth.blood_enabled")
#pragma property g_DirtColor1 "Dirt Red Color" color default(.89627,.799103,.715694,1) group("Weathering") id("cloth.dirt_red_color")
#pragma property g_DirtColor2 "Dirt Green Color" color default(0,0,0,1) group("Weathering") id("cloth.dirt_green_color")
#pragma property g_DirtColor3 "Dirt Blue Color" color default(.305882,.254902,.2,1) group("Weathering") id("cloth.dirt_blue_color")
#pragma property g_BloodColor1 "Blood Red Color" color default(.187821,.07036,.076185,1) group("Weathering") id("cloth.blood_red_color")
#pragma property g_BloodColor2 "Blood Green Color" color default(.327778,.165132,0,1) group("Weathering") id("cloth.blood_green_color")
#pragma property g_BloodColor3 "Blood Blue Color" color default(0,0,0,1) group("Weathering") id("cloth.blood_blue_color")
#pragma property g_DirtIntensity "Dirt RGB Intensity" color default(0,0,0,1) group("Weathering") id("cloth.dirt_intensity")
#pragma property g_BloodIntensity "Blood RGB Intensity" color default(0,0,0,1) group("Weathering") id("cloth.blood_intensity")
#pragma property g_UseMaterialPalette "Use Material ID Palette" float default(0) range(0,1) group("Palette") id("cloth.palette_enabled")
#pragma property g_PaletteColor01 "Red ID" color default(1,1,1,1) group("Palette") id("cloth.palette_red")
#pragma property g_PaletteColor02 "Blue ID" color default(1,1,1,1) group("Palette") id("cloth.palette_blue")
#pragma property g_PaletteColor03 "Green ID" color default(1,1,1,1) group("Palette") id("cloth.palette_green")
#pragma property g_PaletteColor04 "Yellow ID" color default(1,1,1,1) group("Palette") id("cloth.palette_yellow")
#pragma property g_PaletteColor05 "White ID" color default(1,1,1,1) group("Palette") id("cloth.palette_white")
#pragma property g_PaletteColor06 "Black ID" color default(1,1,1,1) group("Palette") id("cloth.palette_black")
#pragma property g_PaletteColor07 "Cyan ID" color default(1,1,1,1) group("Palette") id("cloth.palette_cyan")
#pragma property g_PaletteColor08 "Magenta ID" color default(1,1,1,1) group("Palette") id("cloth.palette_magenta")
#pragma property g_BaseColorFactor "Base Color" color default(1,1,1,1) group("Surface") id("pbr.base_color")
#pragma property g_RoughnessFactor "Roughness" float default(.7) range(0.045,1) group("Surface") id("pbr.roughness")
#pragma property g_ClothMetallicFactor "Metallic Embroidery" float default(0) range(0,1) group("Surface") id("cloth.metallic")
#pragma property g_ClothEmissiveFactor "Emission Color" color default(0,0,0,1) group("Surface") id("cloth.emissive")
#pragma property g_NormalStrength "Normal Strength" float default(1) range(0,2) group("Surface") id("pbr.normal_strength")
#pragma property g_SheenTint "Fiber Sheen" color default(.5,.5,.5,1) group("Cloth") id("cloth.sheen_tint")
#pragma property g_SheenStrength "Sheen Strength" float default(.35) range(0,1) group("Cloth") id("cloth.sheen_strength")
void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface)
{
    const float4 albedo = g_AlbedoMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER, g_AlbedoMap_UV);
    float variance;
    surface.NormalTS = HE_SAMPLE_NORMAL_MAP(g_NormalMap, HE_MATERIAL_DEFAULT_SAMPLER,
        g_NormalMap_UV, g_NormalStrength, variance);
    surface.NormalMipVariance = variance;
    surface.NormalUvSet = g_NormalMap_UV_SET;
    float3 paletteTint = float3(1.0, 1.0, 1.0);
    if (g_UseMaterialPalette > 0.5)
    {
        const float3 id = step(float3(0.5, 0.5, 0.5),
            g_MaterialIdMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER, g_MaterialIdMap_UV).rgb);
        const float r = id.r, g = id.g, b = id.b;
        paletteTint = g_PaletteColor01.rgb * (r * (1.0 - g) * (1.0 - b))
            + g_PaletteColor02.rgb * ((1.0 - r) * (1.0 - g) * b)
            + g_PaletteColor03.rgb * ((1.0 - r) * g * (1.0 - b))
            + g_PaletteColor04.rgb * (r * g * (1.0 - b))
            + g_PaletteColor05.rgb * (r * g * b)
            + g_PaletteColor06.rgb * ((1.0 - r) * (1.0 - g) * (1.0 - b))
            + g_PaletteColor07.rgb * ((1.0 - r) * g * b)
            + g_PaletteColor08.rgb * (r * (1.0 - g) * b);
    }
    float3 weathering = float3(1.0, 1.0, 1.0);
    if (g_UseDirt > 0.5)
    {
        const float3 mask = g_DirtMaskMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER, g_DirtMaskMap_UV).rgb;
        weathering *= saturate(1.0 - mask.r * g_DirtIntensity.r * (1.0 - g_DirtColor1.rgb)
            - mask.g * g_DirtIntensity.g * (1.0 - g_DirtColor2.rgb)
            - mask.b * g_DirtIntensity.b * (1.0 - g_DirtColor3.rgb));
    }
    if (g_UseBlood > 0.5)
    {
        const float3 mask = g_BloodMaskMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER, g_BloodMaskMap_UV).rgb;
        weathering *= saturate(1.0 - mask.r * g_BloodIntensity.r * (1.0 - g_BloodColor1.rgb)
            - mask.g * g_BloodIntensity.g * (1.0 - g_BloodColor2.rgb)
            - mask.b * g_BloodIntensity.b * (1.0 - g_BloodColor3.rgb));
    }
    surface.BaseColor = albedo.rgb * g_BaseColorFactor.rgb * paletteTint * weathering;
    const float2 metallicRoughness = g_ClothSurfaceMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER, g_ClothSurfaceMap_UV).bg;
    surface.Metallic = g_ClothMetallicFactor * metallicRoughness.x;
    surface.PerceptualRoughness = g_RoughnessFactor * metallicRoughness.y;
    surface.Emissive = g_ClothEmissiveFactor.rgb * g_ClothEmissiveMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER, g_ClothEmissiveMap_UV).rgb;
    surface.Opacity = albedo.a * g_BaseColorFactor.a;
    surface.CharacterModel = 4;
    surface.CharacterTint = g_SheenTint.rgb;
    surface.CharacterStrength = g_SheenStrength;
}
