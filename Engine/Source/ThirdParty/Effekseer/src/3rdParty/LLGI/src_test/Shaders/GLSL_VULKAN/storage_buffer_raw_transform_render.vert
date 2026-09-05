#version 430

struct VS_INPUT
{
    vec3 g_position;
    vec2 g_uv;
    vec4 g_color;
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

layout(location = 0) in vec3 input_g_position;
layout(location = 1) in vec2 input_g_uv;
layout(location = 2) in vec4 input_g_color;
layout(location = 0) out vec4 _entryPointOutput_g_color;

vec3 LoadTransformedPosition(uint index, vec3 localPosition)
{
    uint base = index * 80u;
    vec3 c0 = uintBitsToFloat(uvec3(TransformData_1._data[int((base + 32u) >> uint(2))], TransformData_1._data[int((base + 48u) >> uint(2))], TransformData_1._data[int((base + 64u) >> uint(2))]));
    vec3 c1 = uintBitsToFloat(uvec3(TransformData_1._data[int((base + 36u) >> uint(2))], TransformData_1._data[int((base + 52u) >> uint(2))], TransformData_1._data[int((base + 68u) >> uint(2))]));
    vec3 c2 = uintBitsToFloat(uvec3(TransformData_1._data[int((base + 40u) >> uint(2))], TransformData_1._data[int((base + 56u) >> uint(2))], TransformData_1._data[int((base + 72u) >> uint(2))]));
    vec3 c3 = uintBitsToFloat(uvec3(TransformData_1._data[int((base + 44u) >> uint(2))], TransformData_1._data[int((base + 60u) >> uint(2))], TransformData_1._data[int((base + 76u) >> uint(2))]));
    return (((c0 * localPosition.x) + (c1 * localPosition.y)) + (c2 * localPosition.z)) + c3;
}

VS_OUTPUT _main(VS_INPUT _input)
{
    uint param = _input.InstanceId;
    vec3 param_1 = _input.g_position;
    vec3 position = LoadTransformedPosition(param, param_1);
    VS_OUTPUT _output;
    _output.g_position = vec4(position, 1.0);
    bvec4 _172 = bvec4(_input.InstanceId == 0u);
    _output.g_color = vec4(_172.x ? vec4(1.0, 0.0, 0.0, 1.0).x : vec4(0.0, 1.0, 0.0, 1.0).x, _172.y ? vec4(1.0, 0.0, 0.0, 1.0).y : vec4(0.0, 1.0, 0.0, 1.0).y, _172.z ? vec4(1.0, 0.0, 0.0, 1.0).z : vec4(0.0, 1.0, 0.0, 1.0).z, _172.w ? vec4(1.0, 0.0, 0.0, 1.0).w : vec4(0.0, 1.0, 0.0, 1.0).w);
    return _output;
}

void main()
{
    VS_INPUT _input;
    _input.g_position = input_g_position;
    _input.g_uv = input_g_uv;
    _input.g_color = input_g_color;
    _input.InstanceId = uint(gl_InstanceIndex);
    VS_INPUT param = _input;
    VS_OUTPUT flattenTemp = _main(param);
    vec4 _position = flattenTemp.g_position;
    _position.y = -_position.y;
    gl_Position = _position;
    _entryPointOutput_g_color = flattenTemp.g_color;
}

