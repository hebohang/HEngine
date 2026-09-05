#include "Modern/MaterialSurface.hlsli"

#pragma hmaterial Surface Unlit
#pragma hblend Opaque
#pragma hcull None
#pragma hdepthwrite On
#pragma hshadow On

#pragma property g_ColorA "Color A" color default(0.95,0.1,0.03,1) group("Pattern") id("pattern.color_a")
#pragma property g_ColorB "Color B" color default(0.02,0.15,0.8,1) group("Pattern") id("pattern.color_b")
#pragma property g_PatternMode "Pattern Mode" int default(0) range(0,2) group("Pattern") id("pattern.mode")
#pragma property g_PatternScale "Pattern Scale" float default(10) range(1,64) group("Pattern") id("pattern.scale")
#pragma property g_BandCount "Band Count" float default(4) range(2,12) group("Pattern") id("pattern.band_count")
#pragma property g_FlowSpeed "Flow Speed" float default(0) range(-2,2) group("Animation") id("pattern.flow_speed")
#pragma property g_EmissiveStrength "Emissive" float default(0.05) range(0,4) group("Surface") id("pattern.emissive")

void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface)
{
    const float flow = input.TimeSeconds * g_FlowSpeed;
    const float bAnimated = step(0.0001, abs(g_FlowSpeed));
    const float2 animatedUv = input.UV0 + float2(flow * 0.19, -flow * 0.11);
    const float2 cell = floor(animatedUv * g_PatternScale);
    const float checker = abs(fmod(cell.x + cell.y, 2.0));
    const float facing = saturate(dot(normalize(input.NormalWS), normalize(input.ViewWS)));
    const float toon = floor(facing * g_BandCount) / max(g_BandCount - 1.0, 1.0);
    const float toonWave = 0.5 + 0.5 * sin(
        input.PositionWS.y * g_PatternScale * 0.42 -
        flow * 6.2831853 +
        dot(input.PositionWS.xz, float2(0.29, 0.47)));
    const float stripes = 0.5 + 0.5 * sin(
        (input.UV0.x * 1.4 + input.UV0.y) * g_PatternScale * 6.2831853 -
        flow * 6.2831853);
    float blendValue = checker;
    if (g_PatternMode == 1)
        blendValue = lerp(toon, saturate(toon * 0.72 + toonWave * 0.28), bAnimated);
    else if (g_PatternMode == 2)
        blendValue = smoothstep(0.35, 0.65, stripes);
    const float backFaceTint = input.bFrontFace ? 1.0 : 0.45;
    surface.BaseColor = lerp(g_ColorA.rgb, g_ColorB.rgb, blendValue) * backFaceTint;
    const float emissiveMask = lerp(0.45, 1.0, blendValue);
    surface.Emissive = lerp(g_ColorA.rgb, g_ColorB.rgb, blendValue) *
        g_EmissiveStrength * emissiveMask;
}
