#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct VS_INPUT
{
    float3 g_position;
    float2 g_uv;
    float4 g_color;
    uint VertexId;
    uint InstanceId;
};

struct VS_OUTPUT
{
    float4 g_position;
    float4 g_color;
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

struct RibbonRecord_1
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

struct RibbonData
{
    RibbonRecord_1 _data[1];
};

struct HistoryRecord
{
    float3 Position;
    uint ColorTag;
    float3 Tangent;
    uint Sequence;
};

struct HistoryRecord_1
{
    packed_float3 Position;
    uint ColorTag;
    packed_float3 Tangent;
    uint Sequence;
};

struct HistoryData
{
    HistoryRecord_1 _data[1];
};

struct main0_out
{
    float4 _entryPointOutput_g_color [[user(locn0)]];
    float4 gl_Position [[position]];
};

struct main0_in
{
    float3 input_g_position [[attribute(0)]];
    float2 input_g_uv [[attribute(1)]];
    float4 input_g_color [[attribute(2)]];
};

static inline __attribute__((always_inline))
float4 SelectColor(thread const uint& tag, thread const uint& pointIndex)
{
    float tint = (pointIndex == 0u) ? 0.75 : 1.0;
    if (tag == 0u)
    {
        return float4(tint, 0.0, 0.0, 1.0);
    }
    return float4(0.0, tint, 0.0, 1.0);
}

static inline __attribute__((always_inline))
VS_OUTPUT _main(thread const VS_INPUT& _input, const device RibbonData& RibbonData_1, const device HistoryData& HistoryData_1)
{
    RibbonRecord ribbon;
    ribbon.BaseIndex = RibbonData_1._data[_input.InstanceId].BaseIndex;
    ribbon.Count = RibbonData_1._data[_input.InstanceId].Count;
    ribbon.Head = RibbonData_1._data[_input.InstanceId].Head;
    ribbon.Width = RibbonData_1._data[_input.InstanceId].Width;
    ribbon.ColorTag = RibbonData_1._data[_input.InstanceId].ColorTag;
    ribbon.Reserved0 = RibbonData_1._data[_input.InstanceId].Reserved0;
    ribbon.Reserved1 = RibbonData_1._data[_input.InstanceId].Reserved1;
    ribbon.Reserved2 = RibbonData_1._data[_input.InstanceId].Reserved2;
    uint pointIndex = min((_input.VertexId / 2u), (ribbon.Count - 1u));
    uint historyIndex = ribbon.BaseIndex + ((ribbon.Head + pointIndex) % ribbon.Count);
    HistoryRecord history;
    history.Position = float3(HistoryData_1._data[historyIndex].Position);
    history.ColorTag = HistoryData_1._data[historyIndex].ColorTag;
    history.Tangent = float3(HistoryData_1._data[historyIndex].Tangent);
    history.Sequence = HistoryData_1._data[historyIndex].Sequence;
    float side = ((_input.VertexId & 1u) == 0u) ? 1.0 : (-1.0);
    float3 position = history.Position;
    float3 _143 = position;
    float2 _145 = _143.xy + ((history.Tangent.xy * ribbon.Width) * side);
    position.x = _145.x;
    position.y = _145.y;
    VS_OUTPUT _output;
    _output.g_position = float4(position, 1.0);
    uint param = ribbon.ColorTag;
    uint param_1 = pointIndex;
    _output.g_color = SelectColor(param, param_1);
    return _output;
}

vertex main0_out main0(main0_in in [[stage_in]], const device HistoryData& HistoryData_1 [[buffer(10)]], const device RibbonData& RibbonData_1 [[buffer(11)]], uint gl_VertexIndex [[vertex_id]], uint gl_InstanceIndex [[instance_id]])
{
    main0_out out = {};
    VS_INPUT _input;
    _input.g_position = in.input_g_position;
    _input.g_uv = in.input_g_uv;
    _input.g_color = in.input_g_color;
    _input.VertexId = gl_VertexIndex;
    _input.InstanceId = gl_InstanceIndex;
    VS_INPUT param = _input;
    VS_OUTPUT flattenTemp = _main(param, RibbonData_1, HistoryData_1);
    out.gl_Position = flattenTemp.g_position;
    out._entryPointOutput_g_color = flattenTemp.g_color;
    return out;
}

