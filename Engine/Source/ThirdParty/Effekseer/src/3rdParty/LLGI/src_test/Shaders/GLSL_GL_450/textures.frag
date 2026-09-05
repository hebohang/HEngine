#version 450

struct PS_Input
{
    vec4 Pos;
    vec2 UV;
    vec4 Color;
};

layout(binding = 0) uniform sampler2D _97;
layout(binding = 1) uniform sampler2DArray _99;
layout(binding = 2) uniform sampler3D _101;

layout(location = 0) in vec2 Input_UV;
layout(location = 1) in vec4 Input_Color;
layout(location = 0) out vec4 _entryPointOutput;

vec4 _main(PS_Input Input)
{
    if (Input.UV.x < 0.300000011920928955078125)
    {
        return texture(_97, Input.UV);
    }
    else
    {
        if (Input.UV.x < 0.60000002384185791015625)
        {
            return texture(_99, vec3(Input.UV, 1.0));
        }
    }
    return texture(_101, vec3(Input.UV, 0.5));
}

void main()
{
    PS_Input Input;
    Input.Pos = gl_FragCoord;
    Input.UV = Input_UV;
    Input.Color = Input_Color;
    _entryPointOutput = _main(Input);
}

