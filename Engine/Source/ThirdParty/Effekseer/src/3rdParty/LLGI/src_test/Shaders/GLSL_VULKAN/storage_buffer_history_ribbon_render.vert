#version 430

struct VS_INPUT
{
    vec3 g_position;
    vec2 g_uv;
    vec4 g_color;
    uint VertexId;
    uint InstanceId;
};

struct VS_OUTPUT
{
    vec4 g_position;
    vec4 g_color;
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

struct HistoryRecord
{
    vec3 Position;
    uint ColorTag;
    vec3 Tangent;
    uint Sequence;
};

layout(set = 2, binding = 1, std430) readonly buffer RibbonData
{
    RibbonRecord _data[];
} RibbonData_1;

layout(set = 2, binding = 0, std430) readonly buffer HistoryData
{
    HistoryRecord _data[];
} HistoryData_1;

layout(location = 0) in vec3 input_g_position;
layout(location = 1) in vec2 input_g_uv;
layout(location = 2) in vec4 input_g_color;
layout(location = 0) out vec4 _entryPointOutput_g_color;

vec4 SelectColor(uint tag, uint pointIndex)
{
    float tint = (pointIndex == 0u) ? 0.75 : 1.0;
    if (tag == 0u)
    {
        return vec4(tint, 0.0, 0.0, 1.0);
    }
    return vec4(0.0, tint, 0.0, 1.0);
}

VS_OUTPUT _main(VS_INPUT _input)
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
    history.Position = HistoryData_1._data[historyIndex].Position;
    history.ColorTag = HistoryData_1._data[historyIndex].ColorTag;
    history.Tangent = HistoryData_1._data[historyIndex].Tangent;
    history.Sequence = HistoryData_1._data[historyIndex].Sequence;
    float side = ((_input.VertexId & 1u) == 0u) ? 1.0 : (-1.0);
    vec3 position = history.Position;
    vec3 _143 = position;
    vec2 _145 = _143.xy + ((history.Tangent.xy * ribbon.Width) * side);
    position.x = _145.x;
    position.y = _145.y;
    VS_OUTPUT _output;
    _output.g_position = vec4(position, 1.0);
    uint param = ribbon.ColorTag;
    uint param_1 = pointIndex;
    _output.g_color = SelectColor(param, param_1);
    return _output;
}

void main()
{
    VS_INPUT _input;
    _input.g_position = input_g_position;
    _input.g_uv = input_g_uv;
    _input.g_color = input_g_color;
    _input.VertexId = uint(gl_VertexIndex);
    _input.InstanceId = uint(gl_InstanceIndex);
    VS_INPUT param = _input;
    VS_OUTPUT flattenTemp = _main(param);
    vec4 _position = flattenTemp.g_position;
    _position.y = -_position.y;
    gl_Position = _position;
    _entryPointOutput_g_color = flattenTemp.g_color;
}

