#pragma htype ps PS

#include "Shared/BackendPushConstants.hlsli"

struct RmlVaryings
{
    float4 Position : SV_Position;
    float4 Color : COLOR0;
    float2 UV : TEXCOORD0;
};

struct RmlDecoratorConstants
{
    column_major float4x4 Transform;
    float2 Translation;
    float2 Padding;
    uint DecoratorMode;
    float Time;
    float2 Dimensions;
};
HE_DECLARE_PUSH_CONSTANTS(RmlDecoratorConstants, g_RmlDecorator, space3);

#define g_Transform g_RmlDecorator.Transform
#define g_Translation g_RmlDecorator.Translation
#define g_Padding g_RmlDecorator.Padding
#define g_DecoratorMode g_RmlDecorator.DecoratorMode
#define g_Time g_RmlDecorator.Time
#define g_Dimensions g_RmlDecorator.Dimensions

Texture2D<float4> g_Texture : register(t0, space3);
SamplerState g_LinearClamp : register(s0, space3);

float4 PS(RmlVaryings input) : SV_Target0
{
    float4 color = input.Color * g_Texture.Sample(g_LinearClamp, input.UV);

    if (g_DecoratorMode == 1u)
    {
        const float diagonal = frac(input.UV.x + input.UV.y - g_Time * 0.35f);
        const float sheen = smoothstep(0.40f, 0.50f, diagonal)
            * (1.f - smoothstep(0.50f, 0.62f, diagonal));
        color.rgb += sheen * color.a * 0.42f;
        return float4(min(color.rgb, color.a), color.a);
    }

    if (g_DecoratorMode == 2u)
    {
        const float2 dimensions = max(g_Dimensions, float2(1.f, 1.f));
        const float2 pixel = input.UV * dimensions;
        const float2 cell = frac(pixel / 8.f) - 0.5f;
        const float coverage = 1.f - smoothstep(0.22f, 0.47f, length(cell));
        color *= lerp(0.72f, 1.f, coverage);
    }

    return color;
}
