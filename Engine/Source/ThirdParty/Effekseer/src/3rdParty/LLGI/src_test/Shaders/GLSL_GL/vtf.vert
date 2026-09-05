#version 430

out gl_PerVertex
{
    vec4 gl_Position;
};

struct VS_INPUT
{
    vec3 g_position;
    vec2 g_uv;
    vec4 g_color;
};

struct VS_OUTPUT
{
    vec4 g_position;
    vec4 g_color;
};

layout(binding = 0) uniform sampler2D _94;

layout(location = 0) in vec3 input_g_position;
layout(location = 1) in vec2 input_g_uv;
layout(location = 2) in vec4 input_g_color;
layout(location = 0) out vec4 _entryPointOutput_g_color;

VS_OUTPUT _main(VS_INPUT _input)
{
    vec4 c = textureLod(_94, _input.g_uv, 0.0);
    VS_OUTPUT _output;
    _output.g_position = vec4(_input.g_position, 1.0);
    vec4 _51 = _output.g_position;
    vec2 _53 = _51.xy + c.xy;
    _output.g_position.x = _53.x;
    _output.g_position.y = _53.y;
    _output.g_color = _input.g_color;
    return _output;
}

void main()
{
    VS_INPUT _input;
    _input.g_position = input_g_position;
    _input.g_uv = input_g_uv;
    _input.g_color = input_g_color;
    VS_INPUT param = _input;
    VS_OUTPUT flattenTemp = _main(param);
    gl_Position = flattenTemp.g_position;
    _entryPointOutput_g_color = flattenTemp.g_color;
}

