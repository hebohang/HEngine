#include "Modern/MaterialSurface.hlsli"

#pragma hmaterial Surface Unlit
#pragma hblend Mask
#pragma hcull None
#pragma hdepthwrite On
#pragma hshadow On

#pragma property g_ColorA "Color A" color default(0.95,0.12,0.035,1) group("Checker") id("checker.color_a")
#pragma property g_ColorB "Color B" color default(0.025,0.18,0.65,1) group("Checker") id("checker.color_b")
#pragma property g_CheckerScale "Checker Scale" float default(12) range(1,64) group("Checker") id("checker.scale")
#pragma property g_UvOffset "UV Offset" float2 default(0,0) group("Checker") id("checker.uv_offset")
#pragma property g_Invert "Invert Pattern" bool default(false) group("Checker") id("checker.invert")
#pragma property g_SurfaceMap "Surface Map" texture2d default(white) group("Texture") id("checker.surface_map") semantic(BaseColor) colorspace(SRGB) fallback(white)
#pragma property g_TextureInfluence "Texture Influence" float default(0.35) range(0,1) group("Texture") id("checker.texture_influence")

void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface)
{
    const float2 checkerUv = (input.UV0 + g_UvOffset) * g_CheckerScale;
    const float2 cell = floor(checkerUv);
    float checker = fmod(cell.x + cell.y, 2.0);
    checker = g_Invert ? 1.0 - checker : checker;

    const float4 textureSample = g_SurfaceMap.Sample(
        HE_MATERIAL_DEFAULT_SAMPLER,
        input.UV0);
    const float3 proceduralColor = lerp(g_ColorA.rgb, g_ColorB.rgb, checker);
    surface.BaseColor = proceduralColor * lerp(
        1.0.xxx,
        textureSample.rgb,
        g_TextureInfluence);
    surface.Emissive = proceduralColor * lerp(0.04, 0.12, checker);

    // Both depth and shadow permutations execute this same code path.
    surface.Opacity = textureSample.a * lerp(1.0, 0.2, checker);
    surface.AlphaCutoff = 0.5;
}
