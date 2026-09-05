struct VS_INPUT
{
    float3 g_position : POSITION0;
    float2 g_uv : UV0;
    float4 g_color : COLOR0;
    uint InstanceId : SV_InstanceID;
};

struct VS_OUTPUT
{
    float4 g_position : SV_POSITION;
    float4 g_color : COLOR0;
};

ByteAddressBuffer TransformData : register(t0);

float3 LoadTransformedPosition(uint index, float3 localPosition)
{
    const uint base = index * 80;
    const float3 c0 = asfloat(uint3(TransformData.Load(base + 32), TransformData.Load(base + 48), TransformData.Load(base + 64)));
    const float3 c1 = asfloat(uint3(TransformData.Load(base + 36), TransformData.Load(base + 52), TransformData.Load(base + 68)));
    const float3 c2 = asfloat(uint3(TransformData.Load(base + 40), TransformData.Load(base + 56), TransformData.Load(base + 72)));
    const float3 c3 = asfloat(uint3(TransformData.Load(base + 44), TransformData.Load(base + 60), TransformData.Load(base + 76)));
    return c0 * localPosition.x + c1 * localPosition.y + c2 * localPosition.z + c3;
}

VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT output;
    const float3 position = LoadTransformedPosition(input.InstanceId, input.g_position);
    output.g_position = float4(position, 1.0f);
    output.g_color = input.InstanceId == 0u ? float4(1.0f, 0.0f, 0.0f, 1.0f) : float4(0.0f, 1.0f, 0.0f, 1.0f);
    return output;
}
