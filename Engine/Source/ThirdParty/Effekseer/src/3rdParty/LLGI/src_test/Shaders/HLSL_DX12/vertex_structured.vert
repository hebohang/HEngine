struct VS_INPUT{
    float3 g_position : POSITION0;
    float2 g_uv : UV0;
    float4 g_color : COLOR0;
    uint InstanceId : SV_InstanceID;
};
struct VS_OUTPUT{
    float4 g_position : SV_POSITION;
    float4 g_color : COLOR0;
};

struct CS_INPUT{
    float value1;
    float value2;
};

StructuredBuffer<CS_INPUT> read : register(t0);

VS_OUTPUT main(VS_INPUT input){
    VS_OUTPUT output;
    
    output.g_position = float4(input.g_position, 1.0f);
    output.g_position.x += read[input.InstanceId].value1;
    output.g_position.y += read[input.InstanceId].value2;
    output.g_color = input.g_color;
    
    return output;
}
