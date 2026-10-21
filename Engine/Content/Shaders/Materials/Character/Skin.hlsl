#include "Modern/MaterialSurface.hlsli"
#pragma hmaterial Surface Lit
#pragma hblend Opaque
#pragma hcull Back
#pragma hdepthwrite On
#pragma hshadow On
#pragma property g_AlbedoMap "Base Color / Coverage" texture2d default(white) group("Textures") id("pbr.albedo_map") semantic(BaseColor) colorspace(SRGB) fallback(white)
#pragma property g_NormalMap "Normal" texture2d default(normal) group("Textures") id("pbr.normal_map") semantic(Normal) colorspace(Linear) fallback(normal)
#pragma property g_MetallicRoughnessMap "Roughness (Green)" texture2d default(white) group("Textures") id("g_MetallicRoughnessMap") semantic(MetallicRoughness) colorspace(Linear) fallback(white)
#pragma property g_BaseColorFactor "Base Color" color default(1,1,1,1) group("Surface") id("pbr.base_color")
#pragma property g_RoughnessFactor "Roughness" float default(.42) range(0.045,1) group("Surface") id("pbr.roughness")
#pragma property g_UseRoughnessRemap "Use Skin Roughness Remap" float default(0) range(0,1) group("Skin Roughness") id("skin.roughness_remap_enabled")
#pragma property g_RoughnessMin "Minimum Roughness" float default(.2) range(0,1) group("Skin Roughness") id("skin.roughness_min")
#pragma property g_RoughnessMax "Maximum Roughness" float default(.85) range(0,1) group("Skin Roughness") id("skin.roughness_max")
#pragma property g_RoughnessScale "Roughness Scale" float default(1.2) range(0,4) group("Skin Roughness") id("skin.roughness_scale")
#pragma property g_FresnelRoughness "Grazing Roughness Multiplier" float default(.4) range(0,2) group("Skin Roughness") id("skin.fresnel_roughness")
#pragma property g_FresnelRoughnessExponent "Grazing Roughness Exponent" float default(4.2) range(.1,16) group("Skin Roughness") id("skin.fresnel_roughness_exponent")
#pragma property g_NormalStrength "Normal Strength" float default(1) range(0,2) group("Surface") id("pbr.normal_strength")
#pragma property g_ScatterTint "Diffusion RGB Weight" color default(1,.48,.25,1) group("Skin") id("skin.diffusion_tint")
#pragma property g_ScatterStrength "Diffusion Strength" float default(.55) range(0,1) group("Skin") id("skin.diffusion_strength")
#pragma property g_ThicknessMap "Diffusion Mask" texture2d default(white) group("Skin") id("skin.diffusion_mask") semantic(Data) colorspace(Linear) fallback(white)
#pragma property g_SpecularMap "Dielectric Specular" texture2d default(white) group("Skin Specular") id("skin.specular_map") semantic(Data) colorspace(Linear) fallback(white)
#pragma property g_DetailMaskMap "Specular Detail Mask" texture2d default(white) group("Skin Specular") id("skin.specular_detail_mask") semantic(Data) colorspace(Linear) fallback(white)
#pragma property g_UseSpecularMap "Use Specular Map" float default(0) range(0,1) group("Skin Specular") id("skin.specular_enabled")
#pragma property g_SpecularBase "Base Specular Level" float default(.5) range(0,1) group("Skin Specular") id("skin.specular_base")
#pragma property g_SpecularTiling "Specular Tiling" float default(1) range(.1,16) group("Skin Specular") id("skin.specular_tiling")
#pragma property g_DirtMaskMap "Dirt Mask" texture2d default(black) group("Skin Weathering") id("skin.dirt_mask") semantic(Data) colorspace(Linear) fallback(black)
#pragma property g_BloodMaskMap "Blood Mask" texture2d default(black) group("Skin Weathering") id("skin.blood_mask") semantic(Data) colorspace(Linear) fallback(black)
#pragma property g_UseDirt "Use Dirt" float default(0) range(0,1) group("Skin Weathering") id("skin.dirt_enabled")
#pragma property g_UseBlood "Use Blood" float default(0) range(0,1) group("Skin Weathering") id("skin.blood_enabled")
#pragma property g_DirtColor1 "Dirt Red Color" color default(.89627,.799103,.715694,1) group("Skin Weathering") id("skin.dirt_red_color")
#pragma property g_DirtColor2 "Dirt Green Color" color default(.32549,.164706,0,1) group("Skin Weathering") id("skin.dirt_green_color")
#pragma property g_DirtColor3 "Dirt Blue Color" color default(.305882,.254902,.2,1) group("Skin Weathering") id("skin.dirt_blue_color")
#pragma property g_BloodColor1 "Blood Red Color" color default(.187821,.07036,.076185,1) group("Skin Weathering") id("skin.blood_red_color")
#pragma property g_BloodColor2 "Blood Green Color" color default(.327778,.165132,0,1) group("Skin Weathering") id("skin.blood_green_color")
#pragma property g_BloodColor3 "Blood Blue Color" color default(0,0,0,1) group("Skin Weathering") id("skin.blood_blue_color")
#pragma property g_DirtIntensity "Dirt RGB Intensity" color default(1,0,0,1) group("Skin Weathering") id("skin.dirt_intensity")
#pragma property g_BloodIntensity "Blood RGB Intensity" color default(3,2,0,1) group("Skin Weathering") id("skin.blood_intensity")
#pragma property g_ScarAtlasMap "Scar Color Atlas" texture2d default(black) group("Skin Atlas") id("skin.scar_color") semantic(BaseColor) colorspace(SRGB) fallback(black)
#pragma property g_ScarNormalMap "Scar Normal Atlas" texture2d default(normal) group("Skin Atlas") id("skin.scar_normal") semantic(Normal) colorspace(Linear) fallback(normal)
#pragma property g_UseScarAtlas "Use Scar Atlas" float default(0) range(0,1) group("Skin Atlas") id("skin.scar_enabled")
#pragma property g_ScarTint "Scar Tint" color default(1,1,1,1) group("Skin Atlas") id("skin.scar_tint")
#pragma property g_ScarNormalStrength "Scar Normal Strength" float default(1) range(0,2) group("Skin Atlas") id("skin.scar_normal_strength")
#pragma property g_ScarGridX "Atlas Columns" float default(1) range(1,16) group("Skin Atlas") id("skin.scar_grid_x")
#pragma property g_ScarGridY "Atlas Rows" float default(1) range(1,16) group("Skin Atlas") id("skin.scar_grid_y")
#pragma property g_ScarTileNumber "Atlas Tile Number" float default(1) range(1,256) group("Skin Atlas") id("skin.scar_tile")
#pragma property g_ScarScale "Scar Scale" float default(1) range(.01,4) group("Skin Atlas") id("skin.scar_scale")
#pragma property g_ScarOffsetX "Scar Offset X Percent" float default(0) range(-100,100) group("Skin Atlas") id("skin.scar_offset_x")
#pragma property g_ScarOffsetY "Scar Offset Y Percent" float default(0) range(-100,100) group("Skin Atlas") id("skin.scar_offset_y")
#pragma property g_ScarRotation "Scar Rotation Degrees" float default(0) range(-180,180) group("Skin Atlas") id("skin.scar_rotation")

float2 HE_ScarAtlasUV(float2 uv)
{
    const int gridX = max((int)g_ScarGridX, 1);
    const int gridY = max((int)g_ScarGridY, 1);
    const int tile = max((int)g_ScarTileNumber - 1, 0);
    const float2 cell = 1.0 / float2(gridX, gridY);
    const float2 origin = float2(tile % gridX, tile / gridX) * cell;
    const float2 center = origin + cell * 0.5;
    const float scale = 1.0 / max(g_ScarScale, 0.01);
    const float2 offset = float2(g_ScarOffsetX, g_ScarOffsetY) * 0.01 * cell;
    const float2 position = (uv - 0.5) * cell * scale + center + offset * scale;
    const float angle = radians(g_ScarRotation);
    const float c = cos(angle), s = sin(angle);
    const float2 delta = position - center;
    return clamp(center + float2(c * delta.x - s * delta.y,
                                 s * delta.x + c * delta.y), origin, origin + cell);
}
void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface)
{
    const float4 albedo = g_AlbedoMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER, g_AlbedoMap_UV);
    float variance;
    surface.NormalTS = HE_SAMPLE_NORMAL_MAP(g_NormalMap, HE_MATERIAL_DEFAULT_SAMPLER,
        g_NormalMap_UV, g_NormalStrength, variance);
    surface.NormalMipVariance = variance;
    surface.NormalUvSet = g_NormalMap_UV_SET;
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
    surface.BaseColor = albedo.rgb * g_BaseColorFactor.rgb * weathering;
    if (g_UseScarAtlas > 0.5)
    {
        const float2 scarUV = HE_ScarAtlasUV(input.UV0);
        const float4 scar = g_ScarAtlasMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER, scarUV);
        surface.BaseColor *= lerp(float3(1.0, 1.0, 1.0),
            scar.rgb * g_ScarTint.rgb, scar.a);
        float scarVariance;
        const float3 scarNormal = HE_SAMPLE_NORMAL_MAP(g_ScarNormalMap,
            HE_MATERIAL_DEFAULT_SAMPLER, scarUV, g_ScarNormalStrength, scarVariance);
        surface.NormalTS = normalize(lerp(surface.NormalTS, scarNormal, scar.a));
        surface.NormalMipVariance = max(surface.NormalMipVariance,
            scarVariance * scar.a);
    }
    surface.Metallic = 0;
    surface.SpecularLevel = g_UseSpecularMap > 0.5
        ? saturate(lerp(g_SpecularBase,
            g_SpecularMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER,
                g_SpecularMap_UV * g_SpecularTiling).r,
            g_DetailMaskMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER, g_DetailMaskMap_UV).r))
        : 0.5;
    const float roughnessTexel = g_MetallicRoughnessMap.Sample(
        HE_MATERIAL_DEFAULT_SAMPLER, g_MetallicRoughnessMap_UV).g;
    surface.PerceptualRoughness = g_RoughnessFactor * roughnessTexel;
    if (g_UseRoughnessRemap > 0.5)
    {
        // Match the final lighting basis, including UV1 normal maps,
        // orthogonalized tangents and the back face of two-sided skin.
        const float3 normalWS = HE_ResolveMaterialNormal(input,
            surface.NormalTS, surface.NormalUvSet);
        const float grazing = pow(1.0 - saturate(dot(normalWS, input.ViewWS)),
            max(g_FresnelRoughnessExponent, 0.1));
        surface.PerceptualRoughness = roughnessTexel *
            lerp(g_RoughnessMin, g_RoughnessMax, roughnessTexel) *
            g_RoughnessScale * lerp(1.0, g_FresnelRoughness, grazing);
    }
    surface.Emissive = 0;
    surface.Opacity = albedo.a * g_BaseColorFactor.a;
    surface.CharacterModel = 2;
    surface.CharacterTint = g_ScatterTint.rgb;
    surface.CharacterStrength = g_ScatterStrength * g_ThicknessMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER, g_ThicknessMap_UV).r;
}
