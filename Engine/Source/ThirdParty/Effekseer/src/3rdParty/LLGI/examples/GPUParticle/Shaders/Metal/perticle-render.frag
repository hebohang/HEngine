#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct PS_INPUT
{
    float2 UV;
    float4 Color;
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
float4 _main(thread const PS_INPUT& _input, texture2d<float> ParticleTexture_, sampler ParticleSamplerState_)
{
    if (_input.Color.w <= 0.0)
    {
        discard_fragment();
    }
    float4 color = ParticleTexture_.sample(ParticleSamplerState_, _input.UV);
    return color * _input.Color;
}

fragment main0_out main0(main0_in in [[stage_in]], texture2d<float> ParticleTexture_ [[texture(2)]], sampler ParticleSamplerState_ [[sampler(2)]])
{
    main0_out out = {};
    PS_INPUT _input;
    _input.UV = in.input_UV;
    _input.Color = in.input_Color;
    PS_INPUT param = _input;
    float4 _64 = _main(param, ParticleTexture_, ParticleSamplerState_);
    out._entryPointOutput = _64;
    return out;
}

