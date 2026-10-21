#ifndef HE_STATIC_LIGHTMAP_DIRECTIONAL_ATLAS_HLSLI
#define HE_STATIC_LIGHTMAP_DIRECTIONAL_ATLAS_HLSLI

// Imported directional lightmaps store two coefficient layers in the upper
// and lower halves of a linear atlas. The importer supplies each instance's
// rectangle and coefficient scale/add rows.
float3 HE_DecodeDirectionalAtlasLightmap(
    Texture2D<float4> atlas, SamplerState atlasSampler, float2 uv1,
    float3 normalWS, float4 uvScaleBias, float4 scale0, float4 add0,
    float4 scale1, float4 add1, uint encoding = 0u)
{
    const float2 atlasUv = uv1 * uvScaleBias.xy + uvScaleBias.zw;
    // Native lightmaps store E/pi (unit-white Lambert response), linear HDR.
    // Existing callers multiply by albedo/pi, hence restore E here.
    if (encoding == 1u)
        return max(atlas.Sample(atlasSampler, atlasUv).rgb, 0.0f) * 3.14159265f;
    const float2 topUv = float2(atlasUv.x, atlasUv.y * 0.5f);
    const float4 lightmap0 = atlas.Sample(atlasSampler, topUv);
    const float4 lightmap1 = atlas.Sample(atlasSampler,
        topUv + float2(0.0f, 0.5f));
    const float logL = (lightmap0.a + (lightmap1.a - 0.5f) / 255.0f)
        * scale0.a + add0.a;
    const float3 uvw = lightmap0.rgb * lightmap0.rgb * scale0.rgb + add0.rgb;
    const float luminance = exp2(logL) - 0.01858136f;
    const float4 sh = lightmap1 * scale1 + add1;
    // Restore the source coordinate basis before evaluating directionality.
    const float3 sourceNormalYzx = normalWS.zyx;
    const float directionality = max(0.0f,
        dot(sh, float4(sourceNormalYzx, 1.0f)));
    return max(0.0f, luminance * directionality * uvw);
}

#endif
