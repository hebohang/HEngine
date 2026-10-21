// WeChat ES 3.00 baseline. CPU packs matrix rows explicitly (5 float4s).
cbuffer Params : register(b0)
{
    float4 Rows[4];
    float4 Tint;
};
Texture2D<float4> ColorTexture : register(t0);
SamplerState ColorSampler : register(s0);
struct VertexInput
{
    float2 Position : POSITION;
    float2 Uv : TEXCOORD0;
};
struct VertexOutput
{
    float4 Position : SV_Position;
    float2 Uv : TEXCOORD0;
    float4 Color : COLOR0;
};
VertexOutput VSMain(VertexInput input)
{
    VertexOutput output;
    float4 position = float4(input.Position, 0, 1);
    output.Position = float4(dot(Rows[0], position), dot(Rows[1], position), dot(Rows[2], position), dot(Rows[3], position));
    output.Uv = input.Uv;
    output.Color = Tint;
    return output;
}
float4 PSMain(VertexOutput input) : SV_Target0
{
    return ColorTexture.Sample(ColorSampler, input.Uv) * input.Color;
}
