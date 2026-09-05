#include "Modern/MaterialSurface.hlsli"

#pragma hmaterial Surface Lit
#pragma hblend Transparent
#pragma hcull Back
#pragma hdepthwrite Off
#pragma hshadow Off

#pragma property g_BaseColorFactor "Tint" color default(0.1,0.65,0.9,1) group("Surface") id("glass.tint")
#pragma property g_MetallicFactor "Metallic" float default(0) range(0,1) group("Surface") id("glass.metallic")
#pragma property g_RoughnessFactor "Roughness" float default(0.1) range(0.045,1) group("Surface") id("glass.roughness")
#pragma property g_Opacity "Opacity" float default(0.3) range(0.03,0.95) group("Surface") id("glass.opacity")
#pragma property g_HologramStrength "Hologram" float default(0) range(0,1) group("Hologram") id("hologram.strength")
#pragma property g_ScanScale "Scan Scale" float default(24) range(2,80) group("Hologram") id("hologram.scan_scale")
#pragma property g_ScanSpeed "Scan Speed" float default(1.5) range(-6,6) group("Hologram") id("hologram.scan_speed")
#pragma property g_RimIntensity "Rim Intensity" float default(1) range(0,8) group("Hologram") id("hologram.rim")

void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface)
{
    const float facing = saturate(dot(normalize(input.NormalWS), normalize(input.ViewWS)));
    const float rim = pow(1.0 - facing, 3.0);
    const float scanPhase =
        input.PositionWS.y * g_ScanScale - input.TimeSeconds * g_ScanSpeed;
    const float broadScan = 0.5 + 0.5 * sin(scanPhase);
    const float fineScan = pow(saturate(
        0.5 + 0.5 * sin(scanPhase * 0.47 + 1.7)), 12.0);
    const float flicker = 0.86 + 0.14 * sin(
        input.TimeSeconds * (7.0 + abs(g_ScanSpeed) * 1.7) +
        dot(input.PositionWS.xz, float2(1.7, 2.3)));
    const float scan = saturate(broadScan * 0.62 + fineScan * 1.35) * flicker;
    const float hologram = g_HologramStrength;
    surface.BaseColor = g_BaseColorFactor.rgb * lerp(
        1.0,
        0.38 + broadScan * 0.42 + fineScan * 0.35,
        hologram);
    surface.Metallic = g_MetallicFactor;
    surface.PerceptualRoughness = g_RoughnessFactor;
    surface.Emissive = g_BaseColorFactor.rgb * (
        rim * g_RimIntensity + hologram * (0.12 + scan * 2.15));
    surface.Opacity = g_Opacity * lerp(
        1.0,
        0.42 + broadScan * 0.38 + fineScan * 0.28,
        hologram);
}
