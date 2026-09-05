struct VS_INPUT
{
    float3 g_position : POSITION0;
    float2 g_uv : UV0;
    float4 g_color : COLOR0;
    uint VertexId : SV_VertexID;
    uint InstanceId : SV_InstanceID;
};

struct VS_OUTPUT
{
    float4 g_position : SV_POSITION;
    float4 g_color : COLOR0;
};

struct HistoryRecord
{
    float3 Position;
    uint ColorTag;
    float3 Tangent;
    uint Sequence;
};

struct RibbonRecord
{
    uint BaseIndex;
    uint Count;
    uint Head;
    float Width;
    uint ColorTag;
    uint Reserved0;
    uint Reserved1;
    uint Reserved2;
};

StructuredBuffer<HistoryRecord> HistoryData : register(t0);
StructuredBuffer<RibbonRecord> RibbonData : register(t1);

float4 SelectColor(uint tag, uint pointIndex)
{
    float tint = pointIndex == 0u ? 0.75f : 1.0f;
    if (tag == 0u)
    {
        return float4(tint, 0.0f, 0.0f, 1.0f);
    }
    return float4(0.0f, tint, 0.0f, 1.0f);
}

VS_OUTPUT main(VS_INPUT input)
{
    RibbonRecord ribbon = RibbonData[input.InstanceId];
    uint pointIndex = min(input.VertexId / 2u, ribbon.Count - 1u);
    uint historyIndex = ribbon.BaseIndex + ((ribbon.Head + pointIndex) % ribbon.Count);
    HistoryRecord history = HistoryData[historyIndex];

    float side = (input.VertexId & 1u) == 0u ? 1.0f : -1.0f;
    float3 position = history.Position;
    position.xy += history.Tangent.xy * ribbon.Width * side;

    VS_OUTPUT output;
    output.g_position = float4(position, 1.0f);
    output.g_color = SelectColor(ribbon.ColorTag, pointIndex);
    return output;
}
