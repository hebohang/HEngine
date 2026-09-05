#pragma htype vs VS
#pragma htype ps PS

cbuffer Camera : register(b0, space0)
{
    float4x4 g_ViewProjection;
}

Texture2D g_Textures[32] : register(t0, space1);
SamplerState g_LinearWrapSampler : register(s0, space1);

struct VSInput
{
    float3 Position : POSITION;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD;
    float TexIndex : TEXINDEX;
    float TilingFactor : TILINGFACTOR;
};

struct PSInput
{
    float4 PositionHS : SV_POSITION;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD;
    float TexIndex : TEXINDEX;
    float TilingFactor : TILINGFACTOR;
};

PSInput VS(VSInput vIn)
{
    PSInput vOut;
    vOut.PositionHS = mul(g_ViewProjection, float4(vIn.Position, 1.0f));
    vOut.Color = vIn.Color;
    vOut.TexCoord = vIn.TexCoord;
    vOut.TexIndex = vIn.TexIndex;
    vOut.TilingFactor = vIn.TilingFactor;
    return vOut;
}

float4 PS(PSInput pIn) : SV_Target
{
    uint texIndex = (uint)pIn.TexIndex;
    float2 uv = pIn.TexCoord * pIn.TilingFactor;
    float4 texColor = float4(1.0f, 1.0f, 1.0f, 1.0f);

    switch (texIndex)
    {
    case 0: texColor = g_Textures[0].Sample(g_LinearWrapSampler, uv); break;
    case 1: texColor = g_Textures[1].Sample(g_LinearWrapSampler, uv); break;
    case 2: texColor = g_Textures[2].Sample(g_LinearWrapSampler, uv); break;
    case 3: texColor = g_Textures[3].Sample(g_LinearWrapSampler, uv); break;
    case 4: texColor = g_Textures[4].Sample(g_LinearWrapSampler, uv); break;
    case 5: texColor = g_Textures[5].Sample(g_LinearWrapSampler, uv); break;
    case 6: texColor = g_Textures[6].Sample(g_LinearWrapSampler, uv); break;
    case 7: texColor = g_Textures[7].Sample(g_LinearWrapSampler, uv); break;
    case 8: texColor = g_Textures[8].Sample(g_LinearWrapSampler, uv); break;
    case 9: texColor = g_Textures[9].Sample(g_LinearWrapSampler, uv); break;
    case 10: texColor = g_Textures[10].Sample(g_LinearWrapSampler, uv); break;
    case 11: texColor = g_Textures[11].Sample(g_LinearWrapSampler, uv); break;
    case 12: texColor = g_Textures[12].Sample(g_LinearWrapSampler, uv); break;
    case 13: texColor = g_Textures[13].Sample(g_LinearWrapSampler, uv); break;
    case 14: texColor = g_Textures[14].Sample(g_LinearWrapSampler, uv); break;
    case 15: texColor = g_Textures[15].Sample(g_LinearWrapSampler, uv); break;
    case 16: texColor = g_Textures[16].Sample(g_LinearWrapSampler, uv); break;
    case 17: texColor = g_Textures[17].Sample(g_LinearWrapSampler, uv); break;
    case 18: texColor = g_Textures[18].Sample(g_LinearWrapSampler, uv); break;
    case 19: texColor = g_Textures[19].Sample(g_LinearWrapSampler, uv); break;
    case 20: texColor = g_Textures[20].Sample(g_LinearWrapSampler, uv); break;
    case 21: texColor = g_Textures[21].Sample(g_LinearWrapSampler, uv); break;
    case 22: texColor = g_Textures[22].Sample(g_LinearWrapSampler, uv); break;
    case 23: texColor = g_Textures[23].Sample(g_LinearWrapSampler, uv); break;
    case 24: texColor = g_Textures[24].Sample(g_LinearWrapSampler, uv); break;
    case 25: texColor = g_Textures[25].Sample(g_LinearWrapSampler, uv); break;
    case 26: texColor = g_Textures[26].Sample(g_LinearWrapSampler, uv); break;
    case 27: texColor = g_Textures[27].Sample(g_LinearWrapSampler, uv); break;
    case 28: texColor = g_Textures[28].Sample(g_LinearWrapSampler, uv); break;
    case 29: texColor = g_Textures[29].Sample(g_LinearWrapSampler, uv); break;
    case 30: texColor = g_Textures[30].Sample(g_LinearWrapSampler, uv); break;
    case 31: texColor = g_Textures[31].Sample(g_LinearWrapSampler, uv); break;
    default: break;
    }

    return texColor * pIn.Color;
}
