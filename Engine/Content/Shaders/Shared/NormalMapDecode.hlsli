#ifndef HE_NORMAL_MAP_DECODE_HLSLI
#define HE_NORMAL_MAP_DECODE_HLSLI

// Runtime texture metadata, independent of descriptor indexing mode.
// RGB UNORM=0; BC5 UNORM=1; BC5 SNORM=3. Never infer encoding from texels.
static const uint HE_NORMAL_ENCODING_XY = 1u;
static const uint HE_NORMAL_ENCODING_SIGNED = 2u;
static const float HE_MAX_NORMAL_MIP_VARIANCE = 0.35f;

float3 HE_DecodeNormalMap(float4 sampleValue, float normalScale,
    out float mipVariance, uint encoding = 0u)
{
    const bool xyEncoding = (encoding & HE_NORMAL_ENCODING_XY) != 0u;
    float3 normal = (encoding & HE_NORMAL_ENCODING_SIGNED) != 0u
        ? sampleValue.xyz : sampleValue.xyz * 2.0f - 1.0f;
    if (xyEncoding)
        normal.z = sqrt(saturate(1.0f - dot(normal.xy, normal.xy)));
    if (!all(isfinite(normal)))
    {
        mipVariance = 0.0f;
        return float3(0.0f, 0.0f, 1.0f);
    }

    // RGB mips preserve the averaged vector length. BC5 does not store its
    // averaged Z, so no unresolved mip variance can be recovered from it.
    const float averageLength = length(normal);
    mipVariance = xyEncoding ? 0.0f : (averageLength > 1e-4f
        ? min(max(1.0f - averageLength, 0.0f) / averageLength, HE_MAX_NORMAL_MIP_VARIANCE)
        : HE_MAX_NORMAL_MIP_VARIANCE);
    const float scale = isfinite(normalScale) ? normalScale : 1.0f;
    mipVariance = min(mipVariance * min(scale * scale, 65504.0f), HE_MAX_NORMAL_MIP_VARIANCE);

    // glTF normalTextureInfo.scale: decode XYZ, scale XY, then normalize.
    // Reconstructing Z after scaling changes the intended normal direction.
    // https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html
    normal.xy *= scale;
    const float maxComponent = max(max(abs(normal.x), abs(normal.y)), abs(normal.z));
    if (!isfinite(maxComponent) || maxComponent < 1e-12f)
        return float3(0.0f, 0.0f, 1.0f);
    normal /= maxComponent;
    return normal * rsqrt(max(dot(normal, normal), 1e-12f));
}

#endif
