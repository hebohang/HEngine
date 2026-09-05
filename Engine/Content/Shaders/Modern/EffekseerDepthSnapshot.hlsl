#pragma htype vs VS
#pragma htype ps PS

// Pass-local resources use space3. The scene depth texture is created as a
// shader-readable D32 resource; NVRHI exposes its typeless DX12 allocation as
// an R32_FLOAT SRV for this conversion pass.
Texture2D<float> g_SceneDepth : register(t0, space3);

struct FullscreenVaryings
{
    float4 PositionCS : SV_Position;
};

FullscreenVaryings VS(uint vertexId : SV_VertexID)
{
    FullscreenVaryings output;
    const float2 position = float2(
        (vertexId == 2) ? 3.0f : -1.0f,
        (vertexId == 1) ? 3.0f : -1.0f);
    output.PositionCS = float4(position, 0.0f, 1.0f);
    return output;
}

float PS(FullscreenVaryings input) : SV_Target0
{
    return g_SceneDepth.Load(int3(uint2(input.PositionCS.xy), 0));
}
