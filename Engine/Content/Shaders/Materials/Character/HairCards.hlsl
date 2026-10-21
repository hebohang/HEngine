#include "Modern/MaterialSurface.hlsli"
#pragma hmaterial Surface Lit
#pragma hblend Mask
#pragma hcull None
#pragma hdepthwrite On
#pragma hshadow On
#pragma property g_AlbedoMap "Base Color / Coverage" texture2d default(white) group("Textures") id("pbr.albedo_map") semantic(BaseColor) colorspace(SRGB) fallback(white)
#pragma property g_OpacityMap "Separate Coverage" texture2d default(white) group("Textures") id("hair.opacity_map") semantic(Data) colorspace(Linear) fallback(white)
#pragma property g_NormalMap "Normal" texture2d default(normal) group("Textures") id("pbr.normal_map") semantic(Normal) colorspace(Linear) fallback(normal)
#pragma property g_StrandIdMap "Strand ID" texture2d default(white) group("Hair") id("hair.strand_id_map") semantic(Data) colorspace(Linear) fallback(white)
#pragma property g_StrandVariation "Strand Direction Variation" float default(0) range(0,.5) group("Hair") id("hair.strand_variation")
#pragma property g_RootTipMap "Root To Tip Mask" texture2d default(black) group("Hair Color") id("hair.root_tip_map") semantic(Data) colorspace(Linear) fallback(black)
#pragma property g_UseAlbedo "Use Albedo Color" float default(1) range(0,1) group("Hair Color") id("hair.use_albedo")
#pragma property g_UseRootTipGradient "Use Root To Tip Gradient" float default(0) range(0,1) group("Hair Color") id("hair.root_tip_enabled")
#pragma property g_RootColor "Root Color" color default(1,1,1,1) group("Hair Color") id("hair.root_color")
#pragma property g_MidColor "Mid Color" color default(1,1,1,1) group("Hair Color") id("hair.mid_color")
#pragma property g_TipColor "Tip Color" color default(1,1,1,1) group("Hair Color") id("hair.tip_color")
#pragma property g_RootTipMax "Root Tip Mask Maximum" float default(.7) range(.01,1) group("Hair Color") id("hair.root_tip_max")
#pragma property g_RootTipBias "Root Tip Bias" float default(1) range(.1,4) group("Hair Color") id("hair.root_tip_bias")
#pragma property g_BaseColorFactor "Base Color" color default(1,1,1,1) group("Surface") id("pbr.base_color")
#pragma property g_RoughnessFactor "Roughness" float default(.36) range(0.045,1) group("Surface") id("pbr.roughness")
#pragma property g_NormalStrength "Normal Strength" float default(1) range(0,2) group("Surface") id("pbr.normal_strength")
#pragma property g_AlphaCutoff "Coverage Cutoff" float default(.4) range(.05,.95) group("Hair") id("hair.cutoff")
#pragma property g_OpacityMipBias "Coverage Mip Bias" float default(0) range(-3,3) group("Hair") id("hair.opacity_mip_bias")
#pragma property g_OpacityIntensity "Coverage Intensity" float default(1) range(0,4) group("Hair") id("hair.opacity_intensity")
#pragma property g_OpacityThickness "Coverage Thickness" float default(1) range(.1,4) group("Hair") id("hair.opacity_thickness")
#pragma property g_Anisotropy "Strand Response" float default(1) range(0,1) group("Hair") id("hair.anisotropy")
#pragma property g_FlowAngle "UV Strand Angle" float default(1.570796) range(-3.141593,3.141593) group("Hair") id("hair.flow_angle")
void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface)
{
    const float4 albedo = g_AlbedoMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER, g_AlbedoMap_UV);
    float variance;
    surface.NormalTS = HE_SAMPLE_NORMAL_MAP(g_NormalMap, HE_MATERIAL_DEFAULT_SAMPLER,
        g_NormalMap_UV, g_NormalStrength, variance);
    surface.NormalMipVariance = variance;
    surface.NormalUvSet = g_NormalMap_UV_SET;
    float3 hairTint = float3(1.0, 1.0, 1.0);
    if (g_UseRootTipGradient > 0.5)
    {
        const float position = pow(saturate(
            g_RootTipMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER, g_RootTipMap_UV).r
            / max(g_RootTipMax, 0.01)), g_RootTipBias);
        hairTint = lerp(g_RootColor.rgb, g_MidColor.rgb, saturate(position * 2.0));
        hairTint = lerp(hairTint, g_TipColor.rgb, saturate(position * 2.0 - 1.0));
    }
    surface.BaseColor = lerp(float3(1.0, 1.0, 1.0), albedo.rgb,
        saturate(g_UseAlbedo)) * g_BaseColorFactor.rgb * hairTint;
    surface.Metallic = 0;
    surface.PerceptualRoughness = g_RoughnessFactor;
    surface.Emissive = 0;
    const float coverage = saturate(pow(saturate(
        g_OpacityMap.SampleBias(HE_MATERIAL_DEFAULT_SAMPLER, g_OpacityMap_UV, g_OpacityMipBias).r
        * g_OpacityIntensity), max(g_OpacityThickness, 0.1)));
    surface.Opacity = albedo.a * coverage * g_BaseColorFactor.a;
    surface.CharacterModel = 3;
    surface.CharacterStrength = g_Anisotropy;
    // Fibre direction is authored root -> tip, not a facing-dependent shading
    // normal. Keep UV orientation on backfaces; normal mapping stays two-sided.
    HEMaterialInput fibreInput = input;
    fibreInput.bFrontFace = true;
    const float3x3 basis = HE_BuildWorldTbn(fibreInput, g_AlbedoMap_UV_SET);
    // UE's hair-card graph perturbs the tangent with its per-strand ID map.
    // Keep the default at zero for existing materials without that map.
    const float strandId = g_StrandIdMap.SampleBias(HE_MATERIAL_DEFAULT_SAMPLER,
        g_StrandIdMap_UV, -1.0).r;
    const float strandAngle = g_FlowAngle + (strandId * 2.0 - 1.0) * g_StrandVariation;
    surface.StrandDirectionWS = HE_SafeNormalize(cos(strandAngle) * basis[0] + sin(strandAngle) * basis[1]);
    surface.AlphaCutoff = g_AlphaCutoff;
}
