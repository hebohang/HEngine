#version 430

struct PS_Input
{
    vec4 Pos;
    vec2 UV;
    vec2 UV2;
    vec4 Color;
    vec3 WorldN;
    vec3 WorldB;
    vec3 WorldT;
};

struct ParameterData
{
    int EmitCount;
    int EmitPerFrame;
    float EmitOffset;
    uint Padding0;
    vec2 LifeTime;
    uint EmitShapeType;
    uint EmitRotationApplied;
    vec4 EmitShapeData[2];
    vec3 Direction;
    float Spread;
    vec2 InitialSpeed;
    vec2 Damping;
    vec4 AngularOffset[2];
    vec4 AngularVelocity[2];
    vec4 ScaleData1[2];
    vec4 ScaleData2[2];
    vec3 ScaleEasing;
    uint ScaleFlags;
    vec3 Gravity;
    uint Padding2;
    vec3 VortexCenter;
    float VortexRotation;
    vec3 VortexAxis;
    float VortexAttraction;
    float TurbulencePower;
    uint TurbulenceSeed;
    float TurbulenceScale;
    float TurbulenceOctave;
    uint RenderState;
    uint ShapeType;
    uint ShapeData;
    float ShapeSize;
    float Emissive;
    float FadeIn;
    float FadeOut;
    uint MaterialType;
    uvec4 ColorData;
    vec3 ColorEasing;
    uint ColorFlags;
};

struct RenderConstants
{
    vec3 CameraPos;
    uint CoordinateReversed;
    vec3 CameraFront;
    float Reserved1;
    vec3 LightDir;
    float Reserved2;
    vec4 LightColor;
    vec4 LightAmbient;
    mat4 ProjMat;
    mat4 CameraMat;
    mat4x3 BillboardMat;
    mat4x3 YAxisFixedMat;
};

layout(set = 0, binding = 1, std140) uniform cb1
{
    ParameterData paramData;
} _49;

layout(set = 0, binding = 0, std140) uniform cb0
{
    layout(row_major) RenderConstants constants;
} _107;

layout(set = 1, binding = 2) uniform sampler2D Sampler_ColorSamp;
layout(set = 1, binding = 3) uniform sampler2D Sampler_NormalSamp;

layout(location = 0) in vec2 input_UV;
layout(location = 1) in vec2 input_UV2;
layout(location = 2) in vec4 input_Color;
layout(location = 3) in vec3 input_WorldN;
layout(location = 4) in vec3 input_WorldB;
layout(location = 5) in vec3 input_WorldT;
layout(location = 0) out vec4 _entryPointOutput;

vec4 _main(PS_Input _input)
{
    vec4 color = _input.Color * texture(Sampler_ColorSamp, _input.UV);
    vec2 uv2 = _input.UV2;
    if (_49.paramData.MaterialType == 1u)
    {
        vec3 texNormal = (texture(Sampler_NormalSamp, _input.UV).xyz * 2.0) - vec3(1.0);
        vec3 normal = normalize(mat3(vec3(_input.WorldT), vec3(_input.WorldB), vec3(_input.WorldN)) * texNormal);
        float diffuse = max(dot(_107.constants.LightDir, normal), 0.0);
        vec4 _125 = color;
        vec3 _127 = _125.xyz * ((_107.constants.LightColor.xyz * diffuse) + _107.constants.LightAmbient.xyz);
        color.x = _127.x;
        color.y = _127.y;
        color.z = _127.z;
    }
    vec4 _144 = color;
    vec2 _146 = _144.xy + (uv2 * (_49.paramData.FadeIn - _49.paramData.FadeIn));
    color.x = _146.x;
    color.y = _146.y;
    return color;
}

void main()
{
    PS_Input _input;
    _input.Pos = gl_FragCoord;
    _input.UV = input_UV;
    _input.UV2 = input_UV2;
    _input.Color = input_Color;
    _input.WorldN = input_WorldN;
    _input.WorldB = input_WorldB;
    _input.WorldT = input_WorldT;
    _entryPointOutput = _main(_input);
}

