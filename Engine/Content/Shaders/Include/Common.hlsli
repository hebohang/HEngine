#ifndef HENGINE_COMMON_HLSLI
#define HENGINE_COMMON_HLSLI

#include "Math/Constants.hlsli"
#include "Math/Math.hlsli"
#include "Shared/NormalMapDecode.hlsli"

struct DirLightData
{
    float3 direction;
    float intensity;
    float3 color;
    float _DirLightPad;
};

struct PointLightData
{
    float3 position;
    float intensity;
    float3 color;
    float radius;
    float _pointPadding;
    float3 _PointLightPad;
};

cbuffer PerFrameCBuffer : register(b0, space0)
{
    float4x4 g_ViewProjection;
    float3 g_CameraPos;
    float _PerFramePad;
}

cbuffer PerObjectCBuffer : register(b1, space2)
{
    float4x4 g_Model;
    float4x4 g_ModelInvTranspose;
}

cbuffer Lighting : register(b2, space0)
{
    DirLightData g_DirLight[5];
    PointLightData g_PointLight[5];
}

cbuffer ShadowCBuffer : register(b6, space0)
{
    float4x4 g_DirectionalShadowLightViewProjection[4];
    float4x4 g_ShadowCameraView;
    float4 g_DirectionalShadowCascadeSplits;
    float4 g_DirectionalShadowSettings;
    float4 g_DirectionalShadowBias;
    float4 g_DirectionalShadowMisc;
}

#define HENGINE_MAX_INSTANCES 400

cbuffer InstancingCBuffer : register(b7, space2)
{
    float4x4 g_InstanceModels[HENGINE_MAX_INSTANCES];
    float4x4 g_InstanceModelInvTransposes[HENGINE_MAX_INSTANCES];
    uint4 g_InstancingSettings;
}

Texture2DArray g_DirectionalShadowMap : register(t10, space3);

SamplerState g_LinearWrapSampler : register(s0, space0);
SamplerState g_LinearClampSampler : register(s1, space0);
SamplerState g_PointClampSampler : register(s2, space0);
SamplerComparisonState g_ShadowComparisonSampler : register(s3, space0);

struct VertexIn
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 texcoord : TEXCOORD;
    float3 tangent : TANGENT;
    float3 bitangent : BINORMAL;
};

struct VSInputSkinned
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 texcoord : TEXCOORD;
    float3 tangent : TANGENT;
    float3 bitangent : BINORMAL;
    int4 BoneIDs : BONEIDS;
    float4 Weights : WEIGHTS;
};

struct SurfaceVaryings
{
    float4 PositionHS : SV_POSITION;
    float3 PositionWS : TEXCOORD0;
    float3 NormalWS : TEXCOORD1;
    float3 TangentWS : TEXCOORD2;
    float3 BitangentWS : TEXCOORD3;
    float2 TexCoord : TEXCOORD4;
};

float Square(float x)
{
    return x * x;
}

float3 SafeNormalize(float3 v)
{
    const float lenSq = dot(v, v);
    if (lenSq <= 1e-8f)
        return float3(0.0f, 0.0f, 1.0f);

    return v * rsqrt(lenSq);
}

float3 SRGBToLinear(float3 color)
{
    color = saturate(color);
    const float3 low = color / 12.92f;
    const float3 high = pow((color + 0.055f) / 1.055f, 2.4f);
    return lerp(high, low, step(color, 0.04045f));
}

float3 LinearToSRGB(float3 color)
{
    color = saturate(color);
    const float3 low = color * 12.92f;
    const float3 high = 1.055f * pow(color, 1.0f / 2.4f) - 0.055f;
    return lerp(high, low, step(color, 0.0031308f));
}

float3 ToneMapACES(float3 color)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float3x3 BuildWorldTBN(float3 normalWS, float3 tangentWS, float3 bitangentWS)
{
    const float3 N = SafeNormalize(normalWS);
    const float3 orthogonalTangent = tangentWS - N * dot(N, tangentWS);
    const float3 fallbackTangent = (abs(N.z) < 0.999f)
        ? SafeNormalize(cross(float3(0.0f, 0.0f, 1.0f), N))
        : SafeNormalize(cross(float3(0.0f, 1.0f, 0.0f), N));
    const float3 T = SafeNormalize(dot(orthogonalTangent, orthogonalTangent) > 1e-8f ? orthogonalTangent : fallbackTangent);
    float3 B = SafeNormalize(cross(N, T));

    if (dot(B, bitangentWS) < 0.0f)
        B = -B;

    return float3x3(T, B, N);
}

float3 SampleNormalTexture(
    Texture2D normalTexture,
    float2 texCoord,
    float3 normalWS,
    float3 tangentWS,
    float3 bitangentWS,
    float normalStrength,
    out float normalMipVariance,
    uint normalEncoding = 0u)
{
    const float3 normalTS = HE_DecodeNormalMap(
        normalTexture.SampleBias(g_LinearWrapSampler, texCoord, 1.0f),
        normalStrength, normalMipVariance, normalEncoding);

    const float3x3 tbn = BuildWorldTBN(normalWS, tangentWS, bitangentWS);
    return SafeNormalize(normalTS.x * tbn[0] + normalTS.y * tbn[1] + normalTS.z * tbn[2]);
}

// Preserve the older RGB call contract. Production legacy materials are
// extracted into SceneOpaque, which supplies format flags from GPU Scene.
float3 SampleNormalTexture(Texture2D normalTexture, float2 texCoord,
    float3 normalWS, float3 tangentWS, float3 bitangentWS, float normalStrength)
{
    float normalMipVariance;
    return SampleNormalTexture(normalTexture, texCoord, normalWS, tangentWS,
        bitangentWS, normalStrength, normalMipVariance, 0u);
}

float ApplySpecularAntiAliasing(float roughness, float3 normalWS,
    float normalMipVariance = 0.0f)
{
    const float3 dndx = ddx(normalWS);
    const float3 dndy = ddy(normalWS);
    const float normalVariance = min(max(dot(dndx, dndx), dot(dndy, dndy)), 0.35f);
    const float mipVariance = isfinite(normalMipVariance)
        ? clamp(normalMipVariance, 0.0f, HE_MAX_NORMAL_MIP_VARIANCE) : 0.0f;
    return clamp(sqrt(saturate(roughness * roughness + normalVariance + mipVariance)), 0.045f, 1.0f);
}

SurfaceVaryings BuildSurfaceVaryings(
    float3 objectPos,
    float3 objectNormal,
    float3 objectTangent,
    float3 objectBitangent,
    float2 texCoord,
    uint instanceId)
{
    SurfaceVaryings output;
    const bool useInstancing = g_InstancingSettings.x != 0;
    const float4x4 model = useInstancing ? g_InstanceModels[instanceId] : g_Model;
    const float4x4 modelInvTranspose = useInstancing ? g_InstanceModelInvTransposes[instanceId] : g_ModelInvTranspose;
    const float4 worldPos = mul(model, float4(objectPos, 1.0f));
    output.PositionHS = mul(g_ViewProjection, worldPos);
    output.PositionWS = worldPos.xyz;
    output.NormalWS = SafeNormalize(mul((float3x3)modelInvTranspose, objectNormal));
    output.TangentWS = SafeNormalize(mul((float3x3)model, objectTangent));
    output.BitangentWS = SafeNormalize(mul((float3x3)model, objectBitangent));
    output.TexCoord = texCoord;
    return output;
}

#endif
