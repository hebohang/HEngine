#include "Modern/MaterialSurface.hlsli"

#pragma hmaterial Surface Unlit
#pragma hblend Transparent
#pragma hcull None
#pragma hdepthwrite Off
#pragma hshadow Off

#pragma property g_Color "Color" color default(0.15,0.8,1,1) group("Surface") id("pulse.color")
#pragma property g_Opacity "Opacity" float default(0.45) range(0.05,0.95) group("Surface") id("pulse.opacity")
#pragma property g_StripeScale "Stripe Scale" float default(10) range(1,40) group("Pattern") id("pulse.stripe_scale")

void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface)
{
    const float stripe = 0.5 + 0.5 * sin(
        (input.UV0.x + input.UV0.y) * g_StripeScale +
        input.TimeSeconds * 2.0);
    surface.BaseColor = g_Color.rgb * lerp(0.35, 1.0, stripe);
    surface.Emissive = g_Color.rgb * stripe * 0.75;
    surface.Opacity = g_Opacity * lerp(0.55, 1.0, stripe);
}

