#version 430

struct PS_INPUT
{
    vec4 Position;
    vec2 UV;
    vec4 Color;
};

layout(binding = 0, std430) readonly buffer compute
{
    float _data[];
} compute_1;

layout(binding = 1, std140) uniform CB
{
    vec4 offset;
} CBPS0;

layout(location = 0) in vec2 input_UV;
layout(location = 1) in vec4 input_Color;
layout(location = 0) out vec4 _entryPointOutput;

vec4 _main(PS_INPUT _input)
{
    vec4 c = _input.Color + vec4(compute_1._data[0]);
    c.w = 1.0;
    return c;
}

void main()
{
    PS_INPUT _input;
    _input.Position = gl_FragCoord;
    _input.UV = input_UV;
    _input.Color = input_Color;
    PS_INPUT param = _input;
    _entryPointOutput = _main(param);
}

