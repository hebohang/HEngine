#pragma htype vs VS
#pragma htype ps PS

// HEngine binding convention: space0 is reserved for per-frame/per-view data.
// This validation shader intentionally has no resources; matrices are column-major
// under the shared DXC -Zpc compile option.
struct TriangleVertex
{
    float3 Position : POSITION;
    float3 Color : COLOR;
};

struct TriangleVaryings
{
    float4 PositionCS : SV_POSITION;
    float3 Color : COLOR;
};

TriangleVaryings VS(TriangleVertex input)
{
    TriangleVaryings output;
    output.PositionCS = float4(input.Position, 1.0f);
    output.Color = input.Color;
    return output;
}

float4 PS(TriangleVaryings input) : SV_Target0
{
    return float4(input.Color, 1.0f);
}
