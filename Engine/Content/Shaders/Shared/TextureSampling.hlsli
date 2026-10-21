#ifndef HE_TEXTURE_SAMPLING_HLSLI
#define HE_TEXTURE_SAMPLING_HLSLI

struct HETextureSampling
{
    float4 UvRow0;
    float4 UvRow1;
    uint4 Options;
};

float2 HE_TextureUv(HETextureSampling settings, float2 uv0, float2 uv1)
{
    float3 uv = float3(settings.Options.x == 1 ? uv1 : uv0, 1);
    return float2(dot(settings.UvRow0.xyz, uv), dot(settings.UvRow1.xyz, uv));
}

int HE_WrapTexel(int pixel, int extent, uint mode)
{
    if (mode == 1) return clamp(pixel, 0, extent - 1);
    const int period = mode == 2 ? extent * 2 : extent;
    const int wrapped = ((pixel % period) + period) % period;
    return mode == 2 && wrapped >= extent ? period - 1 - wrapped : wrapped;
}

float4 HE_LoadWrapped(Texture2D<float4> textureMap, int2 pixel, uint mip, uint2 size, uint2 wrap)
{
    // SRGB SRV Load decodes each texel before filtering, just like hardware filtering.
    return textureMap.Load(int3(HE_WrapTexel(pixel.x, size.x, wrap.x),
        HE_WrapTexel(pixel.y, size.y, wrap.y), mip));
}

float4 HE_FilterMip(Texture2D<float4> textureMap, float2 uv, uint mip, bool linearFilter, uint2 wrap)
{
    uint width, height, levels;
    textureMap.GetDimensions(mip, width, height, levels);
    // Reduce coordinates before integer conversion, preserving negative/mirror behavior.
    float2 period = float2(wrap.x == 2 ? 2.0 : 1.0, wrap.y == 2 ? 2.0 : 1.0);
    uv.x = wrap.x == 1 ? saturate(uv.x) : uv.x - floor(uv.x / period.x) * period.x;
    uv.y = wrap.y == 1 ? saturate(uv.y) : uv.y - floor(uv.y / period.y) * period.y;
    uint2 size = uint2(width, height);
    if (!linearFilter) return HE_LoadWrapped(textureMap, int2(floor(uv * size)), mip, size, wrap);
    float2 position = uv * size - 0.5;
    int2 origin = int2(floor(position));
    float2 alpha = frac(position);
    float4 top = lerp(HE_LoadWrapped(textureMap, origin, mip, size, wrap),
        HE_LoadWrapped(textureMap, origin + int2(1,0), mip, size, wrap), alpha.x);
    float4 bottom = lerp(HE_LoadWrapped(textureMap, origin + int2(0,1), mip, size, wrap),
        HE_LoadWrapped(textureMap, origin + int2(1,1), mip, size, wrap), alpha.x);
    return lerp(top, bottom, alpha.y);
}

float4 HE_SampleTextureLod(Texture2D<float4> textureMap, SamplerState defaultSampler,
    HETextureSampling settings, float2 uv, float lod)
{
#ifndef HE_TEXTURE_SAMPLING_EXPLICIT_FILTER
    if (settings.Options.y == 0 && settings.Options.z == 0 && settings.Options.w == 261)
        return textureMap.SampleLevel(defaultSampler, uv, max(0.0, lod));
#endif
    uint width, height, levels;
    textureMap.GetDimensions(0, width, height, levels);
    uint minFilter = settings.Options.w & 255;
    bool magnifying = lod <= 0.0;
    bool linearFilter = magnifying ? (settings.Options.w & 256) != 0 : (minFilter == 1 || minFilter == 3 || minFilter == 5);
    float mip = magnifying || minFilter < 2 ? 0.0 : clamp(lod, 0.0, float(levels - 1));
    uint2 wrap = settings.Options.yz;
    if (minFilter < 4 || magnifying)
        return HE_FilterMip(textureMap, uv, uint(floor(mip + 0.5)), linearFilter, wrap);
    uint low = uint(floor(mip));
    return lerp(HE_FilterMip(textureMap, uv, low, linearFilter, wrap),
        HE_FilterMip(textureMap, uv, min(low + 1, levels - 1), linearFilter, wrap), frac(mip));
}

#ifndef HE_TEXTURE_SAMPLING_NO_DERIVATIVES
float4 HE_SampleTexture(Texture2D<float4> textureMap, SamplerState defaultSampler,
    HETextureSampling settings, float2 uv0, float2 uv1)
{
    float2 uv = HE_TextureUv(settings, uv0, uv1);
    if (settings.Options.y == 0 && settings.Options.z == 0 && settings.Options.w == 261)
        return textureMap.Sample(defaultSampler, uv);
    uint width, height, levels;
    textureMap.GetDimensions(0, width, height, levels);
    float2 dx = ddx(uv) * float2(width,height), dy = ddy(uv) * float2(width,height);
    float lod = 0.5 * log2(max(max(dot(dx,dx), dot(dy,dy)), 1e-16));
    return HE_SampleTextureLod(textureMap, defaultSampler, settings, uv, lod);
}
#endif
#endif
