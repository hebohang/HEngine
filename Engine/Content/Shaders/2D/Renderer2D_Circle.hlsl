#pragma htype vs VS
#pragma htype ps PS

cbuffer Camera : register(b0, space0)
{
    float4x4 g_ViewProjection;
}

struct VSInput
{
    float3 WorldPosition : POSITION;
    float3 LocalPosition : LOCALPOS;
    float4 Color : COLOR;
    float Thickness : THICKNESS;
    float Fade : FADE;
};

struct PSInput
{
    float4 PositionHS : SV_POSITION;
    float3 LocalPosition : LOCALPOS;
    float4 Color : COLOR;
    float Thickness : THICKNESS;
    float Fade : FADE;
};

PSInput VS(VSInput vIn)
{
    PSInput vOut;
    vOut.PositionHS = mul(g_ViewProjection, float4(vIn.WorldPosition, 1.0f));
    vOut.LocalPosition = vIn.LocalPosition;
    vOut.Color = vIn.Color;
    vOut.Thickness = vIn.Thickness;
    vOut.Fade = vIn.Fade;
    return vOut;
}

float4 PS(PSInput pIn) : SV_Target
{
    float distance = 1.0f - length(pIn.LocalPosition);
    float circle = smoothstep(0.0f, pIn.Fade, distance);
    circle *= smoothstep(pIn.Thickness + pIn.Fade, pIn.Thickness, distance);
    clip(circle - 0.001f);

    float4 color = pIn.Color;
    color.a *= circle;
    return color;
}
