#include "Modern/MaterialSurface.hlsli"

#pragma hmaterial Surface Lit
#pragma hblend Opaque
#pragma hcull Back
#pragma hdepthwrite On
#pragma hshadow On

#pragma property g_BaseColorFactor "Base Color" color default(0.7,0.72,0.75,1) group("Surface") id("iridescent.base_color")
#pragma property g_MetallicFactor "Metallic" float default(0.75) range(0,1) group("Surface") id("iridescent.metallic")
#pragma property g_RoughnessFactor "Roughness" float default(0.18) range(0.045,1) group("Surface") id("iridescent.roughness")
#pragma property g_IridescenceStrength "Iridescence" float default(0.8) range(0,1) group("Optics") id("iridescent.strength")
#pragma property g_IridescenceScale "Color Cycles" float default(2.5) range(0.5,8) group("Optics") id("iridescent.scale")
#pragma property g_HueSpeed "Hue Speed" float default(0) range(-1,1) group("Optics") id("iridescent.hue_speed")
#pragma property g_LaserIntensity "Laser Intensity" float default(0) range(0,12) group("Laser") id("laser.intensity")
#pragma property g_LaserWidth "Laser Width" float default(0.08) range(0.01,0.35) group("Laser") id("laser.width")
#pragma property g_LaserSpeed "Laser Speed" float default(0.25) range(-2,2) group("Laser") id("laser.speed")

void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface)
{
    const float facing = 1.0 - saturate(dot(normalize(input.NormalWS), normalize(input.ViewWS)));
    const float phase = facing * g_IridescenceScale +
        input.PositionWS.y * 0.08 +
        input.TimeSeconds * g_HueSpeed;
    const float3 rainbow = 0.5 + 0.5 * cos(
        6.2831853 * (phase.xxx + float3(0.0, 0.3333333, 0.6666667)));
    const float scanPosition = frac(input.PositionWS.y * 0.18 - input.TimeSeconds * g_LaserSpeed);
    const float laser = 1.0 - smoothstep(g_LaserWidth, g_LaserWidth * 2.0, abs(scanPosition - 0.5));
    const float shimmer = 0.78 + 0.22 * sin(
        input.TimeSeconds * (1.3 + abs(g_LaserSpeed) * 4.0) +
        dot(input.PositionWS.xz, float2(0.61, 0.43)));
    surface.BaseColor = lerp(
        g_BaseColorFactor.rgb,
        rainbow,
        g_IridescenceStrength * (0.25 + 0.75 * facing));
    surface.Metallic = g_MetallicFactor;
    surface.PerceptualRoughness = g_RoughnessFactor;
    surface.Emissive = rainbow * (
        laser * g_LaserIntensity * shimmer + abs(g_HueSpeed) * 0.35);
}
