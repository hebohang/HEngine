#include "Modern/MaterialSurface.hlsli"

#pragma hmaterial Surface Lit
#pragma hblend Mask
#pragma hcull None
#pragma hdepthwrite On
#pragma hshadow On

#pragma property g_BaseColorFactor "Base Color" color default(0.08,0.2,0.5,1) group("Surface") id("dissolve.base_color")
#pragma property g_EdgeColor "Edge Color" color default(0.1,0.9,1,1) group("Dissolve") id("dissolve.edge_color")
#pragma property g_MetallicFactor "Metallic" float default(0.2) range(0,1) group("Surface") id("dissolve.metallic")
#pragma property g_RoughnessFactor "Roughness" float default(0.35) range(0.045,1) group("Surface") id("dissolve.roughness")
#pragma property g_Dissolve "Dissolve" float default(0.5) range(0.05,0.95) group("Dissolve") id("dissolve.threshold")
#pragma property g_EdgeWidth "Edge Width" float default(0.08) range(0.01,0.3) group("Dissolve") id("dissolve.edge_width")
#pragma property g_NoiseScale "Noise Scale" float default(10) range(2,40) group("Dissolve") id("dissolve.noise_scale")
#pragma property g_FlowSpeed "Flow Speed" float default(0.08) range(-1,1) group("Dissolve") id("dissolve.flow_speed")

void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface)
{
    const float flow = input.TimeSeconds * g_FlowSpeed;
    const float2 animatedUv = input.UV0 + float2(flow * 0.31, -flow * 0.17);
    const float2 cell = floor(animatedUv * g_NoiseScale);
    const float noise = frac(sin(dot(cell, float2(12.9898, 78.233))) * 43758.5453);
    const float field = frac(
        noise * 0.65 + animatedUv.y +
        0.08 * sin(animatedUv.x * 12.0 + flow * 6.2831853));
    const float threshold = clamp(
        g_Dissolve + sin(flow * 3.1) * 0.055 * step(0.0001, abs(g_FlowSpeed)),
        0.05,
        0.95);
    const float edge = 1.0 - smoothstep(0.0, g_EdgeWidth, abs(field - threshold));
    surface.BaseColor = g_BaseColorFactor.rgb;
    surface.Metallic = g_MetallicFactor;
    surface.PerceptualRoughness = g_RoughnessFactor;
    surface.Emissive = g_EdgeColor.rgb * edge * 5.0;
    surface.Opacity = field;
    surface.AlphaCutoff = threshold;
}
