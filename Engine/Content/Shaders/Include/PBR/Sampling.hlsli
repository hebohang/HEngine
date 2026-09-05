#ifndef HENGINE_PBR_SAMPLING_HLSLI
#define HENGINE_PBR_SAMPLING_HLSLI

#include "Include/Common.hlsli"

float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

float2 Hammersley(uint index, uint sampleCount)
{
    return float2(float(index) / float(sampleCount), RadicalInverseVdC(index));
}

float3 TangentToWorld(float3 normalWS, float3 sampleTS)
{
    const float3 upVector = abs(normalWS.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 tangent = SafeNormalize(cross(upVector, normalWS));
    const float3 bitangent = cross(normalWS, tangent);
    return tangent * sampleTS.x + bitangent * sampleTS.y + normalWS * sampleTS.z;
}

float3 ImportanceSampleGGX(float2 xi, float3 normalWS, float roughness)
{
    const float alpha = roughness * roughness;
    const float alpha2 = alpha * alpha;
    const float phi = 2.0f * PI * xi.x;
    const float cosTheta = sqrt((1.0f - xi.y) / max(1.0f + (alpha2 - 1.0f) * xi.y, 1e-6f));
    const float sinTheta = sqrt(max(1.0f - cosTheta * cosTheta, 0.0f));

    const float3 halfTS = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    return SafeNormalize(TangentToWorld(normalWS, halfTS));
}

float3 ImportanceSampleCosineHemisphere(float2 xi, float3 normalWS)
{
    const float phi = 2.0f * PI * xi.x;
    const float cosTheta = sqrt(1.0f - xi.y);
    const float sinTheta = sqrt(xi.y);
    const float3 sampleTS = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    return SafeNormalize(TangentToWorld(normalWS, sampleTS));
}

float2 DirectionToEquirectangularUV(float3 direction)
{
    const float3 normalizedDirection = SafeNormalize(direction);
    const float2 invAtan = float2(0.1591f, 0.3183f);
    float2 uv = float2(atan2(normalizedDirection.z, normalizedDirection.x), asin(normalizedDirection.y));
    uv *= invAtan;
    uv += 0.5f;
    return uv;
}

float2 DirectionToEquirectangularTextureUV(float3 direction)
{
    const float3 normalizedDirection = SafeNormalize(direction);
    const float2 invAtan = float2(0.1591f, 0.3183f);
    float2 uv = float2(atan2(normalizedDirection.z, normalizedDirection.x), -asin(normalizedDirection.y));
    uv *= invAtan;
    uv += 0.5f;
    return uv;
}

float2 CapturePositionToCubeFaceUV(float4 positionHS, float2 captureInvSize)
{
    return positionHS.xy * captureInvSize * 2.0f - 1.0f;
}

float3 CubeFaceUVToDirection(uint faceIndex, float2 faceUV)
{
    switch (faceIndex)
    {
    case 0u: return SafeNormalize(float3(1.0f, -faceUV.y, -faceUV.x));  // +X
    case 1u: return SafeNormalize(float3(-1.0f, -faceUV.y, faceUV.x));  // -X
    case 2u: return SafeNormalize(float3(faceUV.x, 1.0f, faceUV.y));    // +Y
    case 3u: return SafeNormalize(float3(faceUV.x, -1.0f, -faceUV.y));  // -Y
    case 4u: return SafeNormalize(float3(faceUV.x, -faceUV.y, 1.0f));   // +Z
    default: return SafeNormalize(float3(-faceUV.x, -faceUV.y, -1.0f)); // -Z
    }
}

float3 CubeCaptureDirection(uint faceIndex, float4 positionHS, float2 captureInvSize)
{
    return CubeFaceUVToDirection(faceIndex, CapturePositionToCubeFaceUV(positionHS, captureInvSize));
}

#endif
