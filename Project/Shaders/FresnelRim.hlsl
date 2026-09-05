#include "Modern/MaterialSurface.hlsli"

#pragma hmaterial Surface Lit
#pragma hblend Opaque
#pragma hcull Back
#pragma hdepthwrite On
#pragma hshadow On

#pragma property g_BaseColor "Base Color" color default(0.035,0.12,0.42,1) group("Surface") id("surface.base_color")
#pragma property g_Metallic "Metallic" float default(0.15) range(0,1) group("Surface") id("surface.metallic")
#pragma property g_Roughness "Roughness" float default(0.28) range(0.045,1) group("Surface") id("surface.roughness")
#pragma property g_RimColor "Rim Color" color default(0.15,0.65,1,1) group("Rim") id("rim.color")
#pragma property g_RimPower "Rim Power" float default(4) range(0.25,16) group("Rim") id("rim.power")
#pragma property g_RimIntensity "Rim Intensity" float default(3.5) range(0,12) group("Rim") id("rim.intensity")

void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface)
{
    surface.BaseColor = g_BaseColor.rgb;
    surface.Metallic = g_Metallic;
    surface.PerceptualRoughness = g_Roughness;

    const float facing = saturate(dot(
        normalize(input.NormalWS),
        normalize(input.ViewWS)));
    const float rim = pow(1.0 - facing, g_RimPower);
    surface.Emissive += g_RimColor.rgb * rim * g_RimIntensity;
}
