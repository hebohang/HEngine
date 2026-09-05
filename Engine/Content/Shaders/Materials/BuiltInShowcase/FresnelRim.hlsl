#include "Modern/MaterialSurface.hlsli"

#pragma hmaterial Surface Lit
#pragma hblend Opaque
#pragma hcull Back
#pragma hdepthwrite On
#pragma hshadow On

#pragma property g_BaseColorFactor "Base Color" color default(0.03,0.08,0.3,1) group("Surface") id("fresnel.base_color")
#pragma property g_MetallicFactor "Metallic" float default(0.2) range(0,1) group("Surface") id("fresnel.metallic")
#pragma property g_RoughnessFactor "Roughness" float default(0.25) range(0.045,1) group("Surface") id("fresnel.roughness")
#pragma property g_RimColor "Rim Color" color default(0.1,0.7,1,1) group("Rim") id("fresnel.rim_color")
#pragma property g_RimPower "Rim Power" float default(3.5) range(0.25,16) group("Rim") id("fresnel.rim_power")
#pragma property g_RimIntensity "Rim Intensity" float default(4) range(0,16) group("Rim") id("fresnel.rim_intensity")
#pragma property g_PulseSpeed "Pulse Speed" float default(0) range(-2,2) group("Animation") id("fresnel.pulse_speed")
#pragma property g_PulseAmount "Pulse Amount" float default(0) range(0,1) group("Animation") id("fresnel.pulse_amount")
#pragma property g_ScanSpeed "Scan Speed" float default(0) range(-2,2) group("Animation") id("fresnel.scan_speed")
#pragma property g_ScanScale "Scan Scale" float default(18) range(2,64) group("Animation") id("fresnel.scan_scale")

void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface)
{
    const float facing = saturate(dot(normalize(input.NormalWS), normalize(input.ViewWS)));
    const float rim = pow(1.0 - facing, g_RimPower);
    const float positionPhase = dot(input.PositionWS.xz, float2(0.37, 0.53));
    const float pulseWave = 0.5 + 0.5 * sin(
        input.TimeSeconds * g_PulseSpeed * 6.2831853 + positionPhase);
    const float pulse = lerp(
        1.0,
        0.62 + 0.38 * pulseWave,
        g_PulseAmount * step(0.0001, abs(g_PulseSpeed)));
    const float scanWave = 0.5 + 0.5 * sin(
        input.PositionWS.y * g_ScanScale -
        input.TimeSeconds * g_ScanSpeed * 6.2831853 +
        positionPhase);
    const float scan = smoothstep(0.82, 1.0, scanWave) *
        step(0.0001, abs(g_ScanSpeed));
    surface.BaseColor = g_BaseColorFactor.rgb * lerp(0.88, 1.04, pulse);
    surface.Metallic = g_MetallicFactor;
    surface.PerceptualRoughness = g_RoughnessFactor;
    surface.Emissive = g_RimColor.rgb * g_RimIntensity * (
        rim * pulse + scan * (0.18 + 0.52 * (1.0 - facing)));
}
