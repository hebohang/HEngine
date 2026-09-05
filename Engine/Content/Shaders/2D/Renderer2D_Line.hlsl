#pragma htype vs VS
#pragma htype ps PS

cbuffer Camera : register(b0, space0)
{
    float4x4 g_ViewProjection;
}

struct VSInput
{
    float3 Position : POSITION;
    float4 Color : COLOR;
};

struct PSInput
{
    float4 PositionHS : SV_POSITION;
    float4 Color : COLOR;
};

PSInput VS(VSInput vIn)
{
    PSInput vOut;
    vOut.PositionHS = mul(g_ViewProjection, float4(vIn.Position, 1.0f));
    vOut.Color = vIn.Color;
    return vOut;
}

float4 PS(PSInput pIn) : SV_Target
{
    return pIn.Color;
}
