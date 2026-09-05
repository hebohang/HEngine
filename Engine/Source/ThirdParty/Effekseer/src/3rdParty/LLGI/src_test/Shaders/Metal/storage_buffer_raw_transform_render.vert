#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct VS_INPUT
{
    float3 g_position;
    float2 g_uv;
    float4 g_color;
    uint InstanceId;
};

struct VS_OUTPUT
{
    float4 g_position;
    float4 g_color;
};

struct TransformData
{
    uint _data[1];
};

struct main0_out
{
    float4 _entryPointOutput_g_color [[user(locn0)]];
    float4 gl_Position [[position]];
};

struct main0_in
{
    float3 input_g_position [[attribute(0)]];
    float2 input_g_uv [[attribute(1)]];
    float4 input_g_color [[attribute(2)]];
};

static inline __attribute__((always_inline))
float3 LoadTransformedPosition(thread const uint& index, thread const float3& localPosition, const device TransformData& TransformData_1)
{
    uint base = index * 80u;
    float3 c0 = as_type<float3>(uint3(TransformData_1._data[int((base + 32u) >> uint(2))], TransformData_1._data[int((base + 48u) >> uint(2))], TransformData_1._data[int((base + 64u) >> uint(2))]));
    float3 c1 = as_type<float3>(uint3(TransformData_1._data[int((base + 36u) >> uint(2))], TransformData_1._data[int((base + 52u) >> uint(2))], TransformData_1._data[int((base + 68u) >> uint(2))]));
    float3 c2 = as_type<float3>(uint3(TransformData_1._data[int((base + 40u) >> uint(2))], TransformData_1._data[int((base + 56u) >> uint(2))], TransformData_1._data[int((base + 72u) >> uint(2))]));
    float3 c3 = as_type<float3>(uint3(TransformData_1._data[int((base + 44u) >> uint(2))], TransformData_1._data[int((base + 60u) >> uint(2))], TransformData_1._data[int((base + 76u) >> uint(2))]));
    return (((c0 * localPosition.x) + (c1 * localPosition.y)) + (c2 * localPosition.z)) + c3;
}

static inline __attribute__((always_inline))
VS_OUTPUT _main(thread const VS_INPUT& _input, const device TransformData& TransformData_1)
{
    uint param = _input.InstanceId;
    float3 param_1 = _input.g_position;
    float3 position = LoadTransformedPosition(param, param_1, TransformData_1);
    VS_OUTPUT _output;
    _output.g_position = float4(position, 1.0);
    _output.g_color = select(float4(0.0, 1.0, 0.0, 1.0), float4(1.0, 0.0, 0.0, 1.0), bool4(_input.InstanceId == 0u));
    return _output;
}

vertex main0_out main0(main0_in in [[stage_in]], const device TransformData& TransformData_1 [[buffer(10)]], uint gl_InstanceIndex [[instance_id]])
{
    main0_out out = {};
    VS_INPUT _input;
    _input.g_position = in.input_g_position;
    _input.g_uv = in.input_g_uv;
    _input.g_color = in.input_g_color;
    _input.InstanceId = gl_InstanceIndex;
    VS_INPUT param = _input;
    VS_OUTPUT flattenTemp = _main(param, TransformData_1);
    out.gl_Position = flattenTemp.g_position;
    out._entryPointOutput_g_color = flattenTemp.g_color;
    return out;
}

