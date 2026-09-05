#include "Modern/MaterialSurface.hlsli"

#pragma hmaterial Surface Lit
#pragma hblend Opaque
#pragma hcull Back
#pragma hdepthwrite On
#pragma hshadow On

#pragma property g_BaseColorFactor "Base Color" color default(0.11,0.115,0.12,1) group("Surface") id("floor.base_color")
#pragma property g_GridColor "Grid Color" color default(0.28,0.29,0.30,1) group("Grid") id("floor.grid_color")
#pragma property g_MetallicFactor "Metallic" float default(0.03) range(0,1) group("Surface") id("floor.metallic")
#pragma property g_RoughnessFactor "Roughness" float default(0.72) range(0.045,1) group("Surface") id("floor.roughness")
#pragma property g_MajorSpacing "Major Spacing" float default(3.5) range(0.5,12) group("Grid") id("floor.grid_spacing")
#pragma property g_MinorDivisions "Minor Divisions" float default(5) range(1,10) group("Grid") id("floor.minor_divisions")
#pragma property g_FadeStart "Fade Start" float default(32) range(0,100) group("Distance") id("floor.fade_start")
#pragma property g_FadeEnd "Fade End" float default(58) range(1,160) group("Distance") id("floor.fade_end")

float HE_ShowcaseGridLine(
    float2 worldPosition,
    float spacing,
    float lineWidthPixels)
{
    const float2 coordinate = worldPosition / max(spacing, 0.01);
    const float2 footprint = max(fwidth(coordinate), float2(1e-5, 1e-5));
    const float2 nearestLinePixels =
        abs(frac(coordinate + 0.5) - 0.5) / footprint;
    const float gridCoverage = 1.0 - smoothstep(
        lineWidthPixels * 0.5,
        lineWidthPixels * 0.5 + 1.0,
        min(nearestLinePixels.x, nearestLinePixels.y));
    const float nyquistFade = 1.0 - smoothstep(
        0.35,
        0.75,
        max(footprint.x, footprint.y));
    return gridCoverage * nyquistFade;
}

void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface)
{
    const float minorSpacing =
        g_MajorSpacing / max(g_MinorDivisions, 1.0);
    float minorGrid = HE_ShowcaseGridLine(
        input.PositionWS.xz,
        minorSpacing,
        0.65);
    float majorGrid = HE_ShowcaseGridLine(
        input.PositionWS.xz,
        g_MajorSpacing,
        1.5);

    const float cameraDistance = distance(
        g_HECameraPositionAndExposure.xyz,
        input.PositionWS);
    const float fadeRange = max(g_FadeEnd - g_FadeStart, 1.0);
    minorGrid *= 1.0 - smoothstep(
        g_FadeStart,
        g_FadeEnd,
        cameraDistance);
    majorGrid *= 1.0 - smoothstep(
        g_FadeStart + fadeRange * 0.5,
        g_FadeEnd + fadeRange,
        cameraDistance);

    const float topFace = smoothstep(
        0.75,
        0.98,
        saturate(normalize(input.NormalWS).y));
    minorGrid *= topFace;
    majorGrid *= topFace;

    float3 floorColor = lerp(
        g_BaseColorFactor.rgb,
        g_GridColor.rgb,
        minorGrid * 0.38);
    floorColor = lerp(floorColor, g_GridColor.rgb, majorGrid);

    surface.BaseColor = floorColor;
    surface.Metallic = g_MetallicFactor;
    surface.PerceptualRoughness = g_RoughnessFactor;
    surface.Emissive = float3(0.0, 0.0, 0.0);
}
