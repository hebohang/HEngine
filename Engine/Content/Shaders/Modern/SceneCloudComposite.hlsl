#pragma htype vs VS
#pragma htype ps PS
#pragma hvariant MSAA

cbuffer CloudConstants : register(b0, space3)
{
    float4x4 g_InverseViewProjection;
    float4x4 g_RasterViewProjection;
    float4x4 g_PreviousViewProjection;
    float4 g_Camera;
    float4 g_Layer;
    float4 g_Shape;
    float4 g_Wind;
    float4 g_PreviousWind;
    float4 g_Lit;
    float4 g_Shadow;
    float4 g_Sun;
    float4 g_SunColor;
    uint4 g_Dimensions;
    float4 g_Quality;
    uint4 g_State;
};
#if MSAA
Texture2DMS<float> g_Depth : register(t0, space3);
#else
Texture2D<float> g_Depth : register(t0, space3);
#endif
Texture2D<float4> g_Cloud : register(t1, space3);
Texture2D<float2> g_CloudDepth : register(t2, space3);
struct Varyings { float4 Position : SV_Position; float2 Uv : TEXCOORD0; };
Varyings VS(uint id : SV_VertexID)
{
    Varyings o;
    float2 p = float2(id == 2 ? 3 : -1, id == 1 ? 3 : -1);
    o.Position = float4(p, 0, 1);
    o.Uv = float2(p.x, -p.y) * .5 + .5;
    return o;
}
float3 World(float2 uv, float z)
{
    float4 p = mul(g_InverseViewProjection, float4(uv.x * 2 - 1, 1 - uv.y * 2, z, 1));
    return p.xyz / p.w;
}
float4 PS(Varyings input
#if MSAA
    , uint sampleIndex : SV_SampleIndex
#endif
    ) : SV_Target0
{
    float3 origin = g_Camera.w > 0 ? g_Camera.xyz : World(input.Uv, 0);
    float3 direction = normalize(World(input.Uv, .99) - origin);
    float4 clip = mul(g_RasterViewProjection, float4(origin + direction, 1));
    float2 rasterUv = float2(clip.x, -clip.y) / clip.w * .5 + .5;
    uint2 pixel = min(uint2(saturate(rasterUv) * g_Dimensions.xy), g_Dimensions.xy - 1);
#if MSAA
    float z = g_Depth.Load(pixel, sampleIndex);
#else
    float z = g_Depth.Load(int3(pixel, 0));
#endif
    float geometry = z >= .999999 ? g_Layer.z : max(0, dot(World(input.Uv, z) - origin, direction));
    float2 lowPosition = input.Uv * g_Dimensions.zw - .5;
    int2 base = int2(floor(lowPosition));
    float2 fraction = frac(lowPosition);
    float4 cloud = 0;
    float sum = 0;
    [unroll] for (int y = 0; y < 2; ++y)
    [unroll] for (int x = 0; x < 2; ++x)
    {
        int2 p = clamp(base + int2(x,y), 0, int2(g_Dimensions.zw) - 1);
        float2 depths = g_CloudDepth.Load(int3(p, 0));
        float4 sampleValue = g_Cloud.Load(int3(p, 0));
        float weight = (x ? fraction.x : 1 - fraction.x) * (y ? fraction.y : 1 - fraction.y);
        weight *= exp(-abs(depths.y - geometry) / max(1, geometry * .02));
        // A low-resolution cloud behind native geometry never contributes.
        weight *= depths.x <= geometry + 0.5 || sampleValue.a > .999 ? 1 : 0;
        cloud += sampleValue * weight;
        sum += weight;
    }
    if (sum < 1e-5) return 0;
    cloud /= sum;
    if (g_State.w == 1) return float4((1 - cloud.a).xxx, 1);
    if (g_State.w == 2) return float4(cloud.rgb, 1);
    if (g_State.w == 3) return float4(frac(lowPosition / 8), 0, 1);
    // Premultiplied scattering L, blend ONE / INV_SRC_ALPHA: L + T * background.
    return float4(cloud.rgb, 1 - cloud.a);
}
