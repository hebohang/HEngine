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

layout(set = 2, binding = 0, std430) readonly buffer TransformData
{
    uint _data[];
} TransformData_1;

layout(set = 2, binding = 1, std430) readonly buffer AttributeData
{
    uint _data[];
} AttributeData_1;

layout(location = 0) in vec3 input_g_position;
layout(location = 1) in vec2 input_g_uv;
layout(location = 2) in vec4 input_g_color;
layout(location = 0) out vec4 _entryPointOutput_g_color;

vec2 LoadIndexedCorner(uint vertexId)
{
    uint corner = vertexId & 3u;
    if (corner == 0u)
    {
        return vec2(-0.20000000298023223876953125, 0.20000000298023223876953125);
    }
    if (corner == 1u)
    {
        return vec2(0.20000000298023223876953125);
    }
    if (corner == 2u)
    {
        return vec2(0.20000000298023223876953125, -0.20000000298023223876953125);
    }
    return vec2(-0.20000000298023223876953125);
}

vec3 LoadTransformedPosition(uint index, vec3 localPosition)
{
    uint base = index * 80u;
    vec3 c0 = uintBitsToFloat(uvec3(TransformData_1._data[int((base + 32u) >> uint(2))], TransformData_1._data[int((base + 48u) >> uint(2))], TransformData_1._data[int((base + 64u) >> uint(2))]));
    vec3 c1 = uintBitsToFloat(uvec3(TransformData_1._data[int((base + 36u) >> uint(2))], TransformData_1._data[int((base + 52u) >> uint(2))], TransformData_1._data[int((base + 68u) >> uint(2))]));
    vec3 c2 = uintBitsToFloat(uvec3(TransformData_1._data[int((base + 40u) >> uint(2))], TransformData_1._data[int((base + 56u) >> uint(2))], TransformData_1._data[int((base + 72u) >> uint(2))]));
    vec3 c3 = uintBitsToFloat(uvec3(TransformData_1._data[int((base + 44u) >> uint(2))], TransformData_1._data[int((base + 60u) >> uint(2))], TransformData_1._data[int((base + 76u) >> uint(2))]));
    return (((c0 * localPosition.x) + (c1 * localPosition.y)) + (c2 * localPosition.z)) + c3;
}

vec4 SelectColor(uint tag, uint vertexId)
{
    float cornerTint = (vertexId == 0u) ? 0.75 : 1.0;
    if (tag == 0u)
    {
        return vec4(cornerTint, 0.0, 0.0, 1.0);
    }
    if (tag == 1u)
    {
        return vec4(0.0, cornerTint, 0.0, 1.0);
    }
    return vec4(0.0, 0.0, cornerTint, 1.0);
}

VS_OUTPUT _main(VS_INPUT _input)
{
    uint attrBase = _input.InstanceId * 16u;
    int byteAddrTemp = int((attrBase + 0u) >> uint(2));
    vec2 offset = uintBitsToFloat(uvec2(AttributeData_1._data[byteAddrTemp], AttributeData_1._data[byteAddrTemp + 1]));
    float scale = uintBitsToFloat(AttributeData_1._data[int((attrBase + 8u) >> uint(2))]);
    uint colorTag = AttributeData_1._data[int((attrBase + 12u) >> uint(2))];
    vec3 localPosition = _input.g_position;
    uint param = _input.VertexId;
    vec2 _255 = LoadIndexedCorner(param) * scale;
    localPosition.x = _255.x;
    localPosition.y = _255.y;
    uint param_1 = _input.InstanceId;
    vec3 param_2 = localPosition;
    vec3 position = LoadTransformedPosition(param_1, param_2);
    vec3 _268 = position;
    vec2 _270 = _268.xy + offset;
    position.x = _270.x;
    position.y = _270.y;
    VS_OUTPUT _output;
    _output.g_position = vec4(position, 1.0);
    uint param_3 = colorTag;
    uint param_4 = _input.VertexId;
    _output.g_color = SelectColor(param_3, param_4);
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

