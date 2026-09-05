struct VS_INPUT
{
    float3 g_position : POSITION0;
    float2 g_uv : UV0;
    float4 g_color : COLOR0;
    uint VertexId : SV_VertexID;
    uint InstanceId : SV_InstanceID;
};

struct VS_OUTPUT
{
    float4 g_position : SV_POSITION;
    float4 g_color : COLOR0;
};

ByteAddressBuffer TransformData : register(t0);
ByteAddressBuffer AttributeData : register(t1);

float2 LoadIndexedCorner(uint vertexId)
{
    const uint corner = vertexId & 3u;
    if (corner == 0u)
    {
        return float2(-0.2f, 0.2f);
    }
    if (corner == 1u)
    {
        return float2(0.2f, 0.2f);
    }
    if (corner == 2u)
    {
        return float2(0.2f, -0.2f);
    }
    return float2(-0.2f, -0.2f);
}

float3 LoadTransformedPosition(uint index, float3 localPosition)
{
    const uint base = index * 80;
    const float3 c0 = asfloat(uint3(TransformData.Load(base + 32), TransformData.Load(base + 48), TransformData.Load(base + 64)));
    const float3 c1 = asfloat(uint3(TransformData.Load(base + 36), TransformData.Load(base + 52), TransformData.Load(base + 68)));
    const float3 c2 = asfloat(uint3(TransformData.Load(base + 40), TransformData.Load(base + 56), TransformData.Load(base + 72)));
    const float3 c3 = asfloat(uint3(TransformData.Load(base + 44), TransformData.Load(base + 60), TransformData.Load(base + 76)));
    return c0 * localPosition.x + c1 * localPosition.y + c2 * localPosition.z + c3;
}

float4 SelectColor(uint tag, uint vertexId)
{
    const float cornerTint = vertexId == 0u ? 0.75f : 1.0f;
    if (tag == 0u)
    {
        return float4(cornerTint, 0.0f, 0.0f, 1.0f);
    }
    if (tag == 1u)
    {
        return float4(0.0f, cornerTint, 0.0f, 1.0f);
    }
    return float4(0.0f, 0.0f, cornerTint, 1.0f);
}

VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT output;
    const uint attrBase = input.InstanceId * 16u;
    const float2 offset = asfloat(AttributeData.Load2(attrBase + 0));
    const float scale = asfloat(AttributeData.Load(attrBase + 8));
    const uint colorTag = AttributeData.Load(attrBase + 12);

    float3 localPosition = input.g_position;
    localPosition.xy = LoadIndexedCorner(input.VertexId) * scale;

    float3 position = LoadTransformedPosition(input.InstanceId, localPosition);
    position.xy += offset;

    output.g_position = float4(position, 1.0f);
    output.g_color = SelectColor(colorTag, input.VertexId);
    return output;
}
