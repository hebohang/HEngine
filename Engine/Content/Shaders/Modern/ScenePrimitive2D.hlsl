#pragma htype vs VS
#pragma htype ps PS

#include "MaterialDescriptorAccess.hlsli"
#include "Shared/BackendPushConstants.hlsli"

// Draws CPU-extracted 2D scene/debug primitives into the linear HDR target.
// Textures and samplers use the same tier-selected tables as PBR.
cbuffer FrameConstants : register(b0, space0)
{
    float4x4 g_ViewProjection;
    float4x4 g_LightViewProjection;
    float4 g_CameraPositionAndExposure;
    float4 g_DirectionalDirectionAndIntensity;
    float4 g_DirectionalColor;
    float4 g_PointPositionAndIntensity;
    float4 g_PointColorAndRadius;
    float4 g_ShadowSettings;
    float4 g_IblSettings;
};

struct PrimitiveConstants
{
    uint SamplerIndex;
    uint3 Padding;
};
HE_DECLARE_PUSH_CONSTANTS(PrimitiveConstants, g_PrimitiveConstants, space3);

#define g_PrimitiveSamplerIndex g_PrimitiveConstants.SamplerIndex

struct VertexInput
{
    float3 Position : POSITION;
    float3 LineEnd : LINEEND;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD;
    float2 LocalCoord : LOCALCOORD;
    uint TextureIndex : TEXINDEX;
    uint Kind : KIND;
    float Tiling : TILING;
    float Thickness : THICKNESS;
    float Fade : FADE;
    float LineWidth : LINEWIDTH;
};

struct Varyings
{
    float4 PositionCS : SV_Position;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD0;
    float2 LocalCoord : TEXCOORD1;
    nointerpolation uint TextureIndex : TEXCOORD2;
    nointerpolation uint Kind : TEXCOORD3;
    nointerpolation float Thickness : TEXCOORD4;
    nointerpolation float Fade : TEXCOORD5;
};

Varyings VS(VertexInput input)
{
    Varyings output;
    if (input.Kind == 2u)
    {
        const float4 startCS = mul(g_ViewProjection, float4(input.Position, 1.0f));
        const float4 endCS = mul(g_ViewProjection, float4(input.LineEnd, 1.0f));
        const float2 startNdc = startCS.xy / max(abs(startCS.w), 1e-6f);
        const float2 endNdc = endCS.xy / max(abs(endCS.w), 1e-6f);
        const float2 direction = normalize(endNdc - startNdc + float2(1e-8f, 0.0f));
        const float2 normal = float2(-direction.y, direction.x);
        output.PositionCS = lerp(startCS, endCS, input.LocalCoord.x);
        // LineWidth is expressed in pixels. The viewport is not needed here:
        // one NDC unit is conservatively treated as 512 pixels, matching the
        // legacy debug-line scale while keeping this pass resolution-safe.
        output.PositionCS.xy += normal * input.LocalCoord.y *
            input.LineWidth * (1.0f / 512.0f) * output.PositionCS.w;
    }
    else
    {
        output.PositionCS = mul(g_ViewProjection, float4(input.Position, 1.0f));
    }
    output.Color = input.Color;
    output.TexCoord = input.TexCoord * input.Tiling;
    output.LocalCoord = input.LocalCoord;
    output.TextureIndex = input.TextureIndex;
    output.Kind = input.Kind;
    output.Thickness = input.Thickness;
    output.Fade = input.Fade;
    return output;
}

float4 PS(Varyings input) : SV_Target0
{
    Texture2D<float4> textureResource = HE_MATERIAL_TEXTURE(input.TextureIndex);
    SamplerState textureSampler = HE_MATERIAL_SAMPLER(g_PrimitiveSamplerIndex);
    float4 color = textureResource.Sample(textureSampler, input.TexCoord) * input.Color;
    if (input.Kind == 1u)
    {
        const float distanceToEdge = 1.0f - length(input.LocalCoord);
        const float outer = smoothstep(0.0f, max(input.Fade, 1e-5f), distanceToEdge);
        const float inner = smoothstep(
            max(input.Thickness + input.Fade, 1e-5f),
            max(input.Thickness, 1e-5f),
            distanceToEdge);
        color.a *= outer * inner;
    }
    return color;
}
