#define HE_MATERIAL_HAS_VERTEX_OFFSET 1
#include "Modern/MaterialSurface.hlsli"
#pragma hmaterial Surface Lit
#pragma hblend Mask
#pragma hcull None
#pragma hdepthwrite On
#pragma hshadow On
#pragma property g_AlbedoMap "Base Color / Coverage" texture2d default(white) group("Textures") id("pbr.albedo_map") semantic(BaseColor) colorspace(SRGB) fallback(white)
#pragma property g_NormalMap "Normal" texture2d default(normal) group("Textures") id("pbr.normal_map") semantic(Normal) colorspace(Linear) fallback(normal)
#pragma property g_MetallicRoughnessMap "Metallic / Roughness" texture2d default(white) group("Textures") id("pbr.metallic_roughness_map") semantic(MetallicRoughness) colorspace(Linear) fallback(white)
#pragma property g_AoMap "Occlusion" texture2d default(white) group("Textures") id("pbr.ao_map") semantic(Occlusion) colorspace(Linear) fallback(white)
#pragma property g_BaseColorFactor "Base Color" color default(1,1,1,1) group("Surface") id("pbr.base_color")
#pragma property g_RoughnessFactor "Roughness" float default(.8) range(.045,1) group("Surface") id("pbr.roughness")
#pragma property g_MetallicFactor "Metallic" float default(0) range(0,1) group("Surface") id("pbr.metallic")
#pragma property g_AoStrength "Occlusion Strength" float default(1) range(0,1) group("Surface") id("pbr.ao_strength")
#pragma property g_NormalStrength "Normal Strength" float default(1) range(0,2) group("Surface") id("pbr.normal_strength")
#pragma property g_AlphaCutoff "Coverage Cutoff" float default(.5) range(0,1) group("Foliage") id("foliage.alpha_cutoff")
#pragma property g_SubsurfaceColor "Transmission Color" color default(.5,.3,.1,1) group("Foliage") id("foliage.subsurface_color")
#pragma property g_SubsurfaceBoost "Transmission Boost" float default(1) range(0,8) group("Foliage") id("foliage.subsurface_boost")
#pragma property g_TransmissionStrength "Transmission Strength" float default(1) range(0,1) group("Foliage") id("foliage.transmission_strength")
#pragma property g_FuzzyCoreDarkness "Core Darkness" float default(.2) range(0,1) group("Foliage") id("foliage.fuzzy_core_darkness")
#pragma property g_FuzzyEdgeBrightness "Edge Brightness" float default(2) range(0,4) group("Foliage") id("foliage.fuzzy_edge_brightness")
#pragma property g_FuzzyPower "Edge Power" float default(6) range(1,16) group("Foliage") id("foliage.fuzzy_power")
#pragma property g_FuzzyStrength "Fuzzy Amount" float default(1) range(0,1) group("Foliage") id("foliage.fuzzy_strength")
#pragma property g_WindAmplitude "Wind Amplitude (m)" float default(0) range(0,.5) group("Wind") id("foliage.wind_amplitude")
#pragma property g_WindAngle "Wind Angle (rad)" float default(0) range(-6.2832,6.2832) group("Wind") id("foliage.wind_angle")
#pragma property g_WindCyclesPerSecond "Wind Cycles / Second" float default(.4) range(0,4) group("Wind") id("foliage.wind_cycles_per_second")
#pragma property g_WindWavelength "Wind Wavelength (m)" float default(1) range(.1,20) group("Wind") id("foliage.wind_wavelength")

float3 EvaluateMaterialVertexOffset(
    float3 positionWS, float2 uv0, float4 color, float timeSeconds)
{
    const float amplitude = max(g_WindAmplitude, 0.0f);
    if (amplitude <= 0.0f)
        return 0.0f;
    float sine, cosine;
    sincos(g_WindAngle, sine, cosine);
    const float2 direction = float2(cosine, sine);
    const float phase = 6.2831853f * (
        dot(positionWS.xz, direction) / max(g_WindWavelength, 0.1f) +
        timeSeconds * max(g_WindCyclesPerSecond, 0.0f));
    // World position decorrelates neighboring cards; vertex color remains
    // available to an imported foliage mesh without being required by one.
    const float weight = saturate(color.a);
    const float sway = sin(phase) + 0.3f * sin(phase * 1.91f + positionWS.y);
    return float3(direction.x * sway, 0.18f * sin(phase * 1.37f),
                  direction.y * sway) * (amplitude * weight);
}

void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface)
{
    const float4 albedo = g_AlbedoMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER, g_AlbedoMap_UV);
    float variance;
    surface.NormalTS = HE_SAMPLE_NORMAL_MAP(g_NormalMap, HE_MATERIAL_DEFAULT_SAMPLER,
        g_NormalMap_UV, g_NormalStrength, variance);
    surface.NormalMipVariance = variance;
    surface.NormalUvSet = g_NormalMap_UV_SET;
    const float4 orm = g_MetallicRoughnessMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER,
        g_MetallicRoughnessMap_UV);
    // View-angle tint uses the two-sided geometric normal independently of
    // the normal map.
    const float3 geometricNormal = normalize(input.NormalWS) *
        (input.bFrontFace ? 1.0f : -1.0f);
    const float nDotV = saturate(dot(geometricNormal, input.ViewWS));
    const float fuzzy = 1.0f - saturate(g_FuzzyCoreDarkness) * nDotV +
        max(g_FuzzyEdgeBrightness, 0.0f) * pow(1.0f - nDotV, max(g_FuzzyPower, 1.0f));
    surface.BaseColor = albedo.rgb * g_BaseColorFactor.rgb *
        lerp(1.0f, fuzzy, saturate(g_FuzzyStrength));
    surface.Metallic = saturate(g_MetallicFactor * orm.b);
    surface.PerceptualRoughness = clamp(g_RoughnessFactor * orm.g, .045f, 1.0f);
    surface.AO = lerp(1.0f,
        g_AoMap.Sample(HE_MATERIAL_DEFAULT_SAMPLER, g_AoMap_UV).r,
        saturate(g_AoStrength));
    surface.Opacity = albedo.a * g_BaseColorFactor.a;
    surface.AlphaCutoff = g_AlphaCutoff;
    surface.CharacterModel = 7u;
    // Keep the transmission tint separate from the view-tinted base color.
    surface.CharacterTint = saturate(albedo.rgb * g_SubsurfaceColor.rgb *
        g_SubsurfaceColor.rgb * max(g_SubsurfaceBoost, 0.0f));
    surface.CharacterStrength = saturate(g_TransmissionStrength);
}
