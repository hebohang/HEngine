#pragma htype vs VS
#pragma htype ps PS

#include "Shared/BackendPushConstants.hlsli"

struct RmlVertex
{
    float2 Position : POSITION;
    float4 Color : COLOR0;
    float2 UV : TEXCOORD0;
};

struct RmlVaryings
{
    float4 Position : SV_Position;
    float4 Color : COLOR0;
    float2 UV : TEXCOORD0;
};

// Pass-local RmlUi state follows the engine binding convention in space3.
struct RmlProjectionConstants
{
    column_major float4x4 Transform;
    float2 Translation;
    float2 Padding;
};
HE_DECLARE_PUSH_CONSTANTS(RmlProjectionConstants, g_RmlProjection, space3);

#define g_Transform g_RmlProjection.Transform
#define g_Translation g_RmlProjection.Translation
#define g_Padding g_RmlProjection.Padding

Texture2D<float4> g_Texture : register(t0, space3);
SamplerState g_LinearClamp : register(s0, space3);

RmlVaryings VS(RmlVertex input)
{
    RmlVaryings output;
    const float2 translatedPosition = input.Position + g_Translation;
    output.Position = mul(g_Transform, float4(translatedPosition, 0.f, 1.f));
    output.Color = input.Color;
    output.UV = input.UV;
    return output;
}

float4 PS(RmlVaryings input) : SV_Target0
{
    // RmlUi vertices and generated textures are premultiplied-alpha data.
    return input.Color * g_Texture.Sample(g_LinearClamp, input.UV);
}
