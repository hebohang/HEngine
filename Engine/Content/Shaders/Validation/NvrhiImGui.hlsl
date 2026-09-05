#pragma htype vs VS
#pragma htype ps PS

#include "Shared/BackendPushConstants.hlsli"

struct ImGuiVertex
{
    float2 Position : POSITION;
    float2 UV : TEXCOORD0;
    float4 Color : COLOR0;
};

struct ImGuiVaryings
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
    float4 Color : COLOR0;
};

// Pass-local UI state follows the engine binding convention in space3.
// Four 32-bit values intentionally fit in a single NVRHI push-constant block.
struct ImGuiProjectionConstants
{
    float2 Scale;
    float2 Translate;
};
HE_DECLARE_PUSH_CONSTANTS(ImGuiProjectionConstants, g_ImGuiProjection, space3);

#define g_Scale g_ImGuiProjection.Scale
#define g_Translate g_ImGuiProjection.Translate

Texture2D<float4> g_Texture : register(t0, space3);
SamplerState g_LinearClamp : register(s0, space3);

ImGuiVaryings VS(ImGuiVertex input)
{
    ImGuiVaryings output;
    output.Position = float4(input.Position * g_Scale + g_Translate, 0.0f, 1.0f);
    output.UV = input.UV;
    output.Color = input.Color;
    return output;
}

float4 PS(ImGuiVaryings input) : SV_Target0
{
    return input.Color * g_Texture.Sample(g_LinearClamp, input.UV);
}
