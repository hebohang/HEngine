#pragma htype vs VS
#pragma htype ps PS

#include "Shared/BackendPushConstants.hlsli"

struct FullscreenVaryings
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

struct RmlPostprocessConstants
{
    uint Mode;
    float Opacity;
    float Sigma;
    float Padding0;
    float2 InverseTextureSize;
    float2 UvMin;
    float2 UvMax;
    float2 OffsetUv;
    float4 ShadowColor;
    column_major float4x4 ColorMatrix;
};
HE_DECLARE_PUSH_CONSTANTS(RmlPostprocessConstants, g_RmlPostprocess, space3);

#define g_Mode g_RmlPostprocess.Mode
#define g_Opacity g_RmlPostprocess.Opacity
#define g_Sigma g_RmlPostprocess.Sigma
#define g_Padding0 g_RmlPostprocess.Padding0
#define g_InverseTextureSize g_RmlPostprocess.InverseTextureSize
#define g_UvMin g_RmlPostprocess.UvMin
#define g_UvMax g_RmlPostprocess.UvMax
#define g_OffsetUv g_RmlPostprocess.OffsetUv
#define g_ShadowColor g_RmlPostprocess.ShadowColor
#define g_ColorMatrix g_RmlPostprocess.ColorMatrix

Texture2D<float4> g_Source : register(t0, space3);
Texture2D<float4> g_Secondary : register(t1, space3);
SamplerState g_LinearClamp : register(s0, space3);

FullscreenVaryings VS(uint vertexId : SV_VertexID)
{
    FullscreenVaryings output;
    output.UV = float2((vertexId << 1) & 2, vertexId & 2);
    output.Position = float4(output.UV * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);
    return output;
}

float4 SampleSource(float2 uv)
{
    return g_Source.Sample(g_LinearClamp, clamp(uv, g_UvMin, g_UvMax));
}

float4 PS(FullscreenVaryings input) : SV_Target0
{
    const float2 uv = input.UV;
    const float4 source = SampleSource(uv);

    if (g_Mode == 1u)
        return source * g_Opacity;

    if (g_Mode == 2u)
    {
        // A bounded nine-tap kernel. Sigma controls the sample radius but can
        // never grow the amount of shader work performed by untrusted markup.
        const float2 radius = g_InverseTextureSize * max(g_Sigma, 0.f);
        float4 result = source * 0.20f;
        result += SampleSource(uv + float2( radius.x, 0.f)) * 0.10f;
        result += SampleSource(uv + float2(-radius.x, 0.f)) * 0.10f;
        result += SampleSource(uv + float2(0.f,  radius.y)) * 0.10f;
        result += SampleSource(uv + float2(0.f, -radius.y)) * 0.10f;
        result += SampleSource(uv + float2( radius.x,  radius.y)) * 0.10f;
        result += SampleSource(uv + float2(-radius.x,  radius.y)) * 0.10f;
        result += SampleSource(uv + float2( radius.x, -radius.y)) * 0.10f;
        result += SampleSource(uv + float2(-radius.x, -radius.y)) * 0.10f;
        return result;
    }

    if (g_Mode == 3u)
    {
        const float2 shadowUv = uv - g_OffsetUv;
        const float2 radius = g_InverseTextureSize * max(g_Sigma, 0.f);
        float shadowAlpha = SampleSource(shadowUv).a * 0.20f;
        shadowAlpha += SampleSource(shadowUv + float2( radius.x, 0.f)).a * 0.10f;
        shadowAlpha += SampleSource(shadowUv + float2(-radius.x, 0.f)).a * 0.10f;
        shadowAlpha += SampleSource(shadowUv + float2(0.f,  radius.y)).a * 0.10f;
        shadowAlpha += SampleSource(shadowUv + float2(0.f, -radius.y)).a * 0.10f;
        shadowAlpha += SampleSource(shadowUv + float2( radius.x,  radius.y)).a * 0.10f;
        shadowAlpha += SampleSource(shadowUv + float2(-radius.x,  radius.y)).a * 0.10f;
        shadowAlpha += SampleSource(shadowUv + float2( radius.x, -radius.y)).a * 0.10f;
        shadowAlpha += SampleSource(shadowUv + float2(-radius.x, -radius.y)).a * 0.10f;
        const float4 shadow = g_ShadowColor * shadowAlpha;
        return source + shadow * (1.f - source.a);
    }

    if (g_Mode == 4u)
    {
        // RGB-only CSS matrices can operate directly in premultiplied space;
        // their constant column is multiplied by source alpha through w.
        const float3 transformed = mul(g_ColorMatrix, source).rgb;
        return float4(clamp(transformed, 0.f, source.a), source.a);
    }

    if (g_Mode == 5u)
    {
        const float maskAlpha = g_Secondary.Sample(g_LinearClamp, uv).a;
        return source * maskAlpha;
    }

    return source;
}
