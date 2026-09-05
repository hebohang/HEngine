#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct VS_INPUT
{
    float3 g_position;
    float2 g_uv;
    float4 g_color;
    uint VertexId;
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
float2 LoadIndexedCorner(thread const uint& vertexId)
{
    uint corner = vertexId & 3u;
    if (corner == 0u)
    {
        return float2(-0.20000000298023223876953125, 0.20000000298023223876953125);
    }
    if (corner == 1u)
    {
        return float2(0.20000000298023223876953125);
    }
    if (corner == 2u)
    {
        return float2(0.20000000298023223876953125, -0.20000000298023223876953125);
    }
    return float2(-0.20000000298023223876953125);
}

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
float4 SelectColor(thread const uint& tag, thread const uint& vertexId)
{
    float cornerTint = (vertexId == 0u) ? 0.75 : 1.0;
    if (tag == 0u)
    {
        return float4(cornerTint, 0.0, 0.0, 1.0);
    }
    if (tag == 1u)
    {
        return float4(0.0, cornerTint, 0.0, 1.0);
    }
    return float4(0.0, 0.0, cornerTint, 1.0);
}

static inline __attribute__((always_inline))
VS_OUTPUT _main(thread const VS_INPUT& _input, const device TransformData& TransformData_1, const device TransformData& AttributeData)
{
    uint attrBase = _input.InstanceId * 16u;
    int byteAddrTemp = int((attrBase + 0u) >> uint(2));
    float2 offset = as_type<float2>(uint2(AttributeData._data[byteAddrTemp], AttributeData._data[byteAddrTemp + 1]));
    float scale = as_type<float>(AttributeData._data[int((attrBase + 8u) >> uint(2))]);
    uint colorTag = AttributeData._data[int((attrBase + 12u) >> uint(2))];
    float3 localPosition = _input.g_position;
    uint param = _input.VertexId;
    float2 _255 = LoadIndexedCorner(param) * scale;
    localPosition.x = _255.x;
    localPosition.y = _255.y;
    uint param_1 = _input.InstanceId;
    float3 param_2 = localPosition;
    float3 position = LoadTransformedPosition(param_1, param_2, TransformData_1);
    float3 _268 = position;
    float2 _270 = _268.xy + offset;
    position.x = _270.x;
    position.y = _270.y;
    VS_OUTPUT _output;
    _output.g_position = float4(position, 1.0);
    uint param_3 = colorTag;
    uint param_4 = _input.VertexId;
    _output.g_color = SelectColor(param_3, param_4);
    return _output;
}

vertex main0_out main0(main0_in in [[stage_in]], const device TransformData& TransformData_1 [[buffer(10)]], const device TransformData& AttributeData [[buffer(11)]], uint gl_VertexIndex [[vertex_id]], uint gl_InstanceIndex [[instance_id]])
{
    main0_out out = {};
    VS_INPUT _input;
    _input.g_position = in.input_g_position;
    _input.g_uv = in.input_g_uv;
    _input.g_color = in.input_g_color;
    _input.VertexId = gl_VertexIndex;
    _input.InstanceId = gl_InstanceIndex;
    VS_INPUT param = _input;
    VS_OUTPUT flattenTemp = _main(param, TransformData_1, AttributeData);
    out.gl_Position = flattenTemp.g_position;
    out._entryPointOutput_g_color = flattenTemp.g_color;
    return out;
}

