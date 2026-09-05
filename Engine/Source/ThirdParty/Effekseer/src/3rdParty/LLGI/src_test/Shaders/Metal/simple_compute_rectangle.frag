#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct PS_INPUT
{
    float4 Position;
    float2 UV;
    float4 Color;
};

struct compute
{
    float _data[1];
};

struct CB
{
    float4 offset;
};

struct main0_out
{
    float4 _entryPointOutput [[color(0)]];
};

struct main0_in
{
    float2 input_UV [[user(locn0)]];
    float4 input_Color [[user(locn1)]];
};

static inline __attribute__((always_inline))
float4 _main(thread const PS_INPUT& _input, const device compute& compute0)
{
    float4 c = _input.Color + float4(compute0._data[0]);
    c.w = 1.0;
    return c;
}

fragment main0_out main0(main0_in in [[stage_in]], const device compute& compute0 [[buffer(10)]], float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    PS_INPUT _input;
    _input.Position = gl_FragCoord;
    _input.UV = in.input_UV;
    _input.Color = in.input_Color;
    PS_INPUT param = _input;
    out._entryPointOutput = _main(param, compute0);
    return out;
}

