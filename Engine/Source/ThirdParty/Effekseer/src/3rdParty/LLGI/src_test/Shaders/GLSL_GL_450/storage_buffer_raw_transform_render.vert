#version 450
#ifdef GL_ARB_shader_draw_parameters
#extension GL_ARB_shader_draw_parameters : enable
#endif

out gl_PerVertex
{
    vec4 gl_Position;
};

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

layout(binding = 0, std430) readonly buffer TransformData
{
    uint _data[];
} TransformData_1;

layout(location = 0) in vec3 input_g_position;
layout(location = 1) in vec2 input_g_uv;
layout(location = 2) in vec4 input_g_color;
#ifdef GL_ARB_shader_draw_parameters
#define SPIRV_Cross_BaseInstance gl_BaseInstanceARB
#else
uniform int SPIRV_Cross_BaseInstance;
#endif
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
    _output.g_color = mix(vec4(0.0, 1.0, 0.0, 1.0), vec4(1.0, 0.0, 0.0, 1.0), bvec4(_input.InstanceId == 0u));
    return _output;
}

void main()
{
    VS_INPUT _input;
    _input.g_position = input_g_position;
    _input.g_uv = input_g_uv;
    _input.g_color = input_g_color;
    _input.InstanceId = uint((gl_InstanceID + SPIRV_Cross_BaseInstance));
    VS_INPUT param = _input;
    VS_OUTPUT flattenTemp = _main(param);
    gl_Position = flattenTemp.g_position;
    _entryPointOutput_g_color = flattenTemp.g_color;
}
