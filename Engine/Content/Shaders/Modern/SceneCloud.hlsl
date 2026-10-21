#pragma htype cs CS

cbuffer CloudConstants : register(b0, space3)
{
    float4x4 g_InverseViewProjection;
    float4x4 g_RasterViewProjection;
    float4x4 g_PreviousViewProjection;
    float4 g_Camera;
    float4 g_Layer; // bottom, thickness, maximum distance, shape scale
    float4 g_Shape; // coverage, extinction, roundness, erosion
    float4 g_Wind;
    float4 g_PreviousWind;
    float4 g_Lit;
    float4 g_Shadow;
    float4 g_Sun;
    float4 g_SunColor;
    uint4 g_Dimensions;
    float4 g_Quality; // main steps, light steps, detail distance, history weight
    uint4 g_State; // operation, sequence, history valid, debug
};
Texture2D<float> g_Depth : register(t0, space3);
Texture3D<float> g_Noise : register(t1, space3);
Texture2D<float4> g_Current : register(t2, space3);
Texture2D<float2> g_CurrentDepth : register(t3, space3);
Texture2D<float4> g_History : register(t4, space3);
Texture2D<float2> g_HistoryDepth : register(t5, space3);
SamplerState g_Wrap : register(s0, space3);
RWTexture2D<float4> g_Output : register(u0, space3);
RWTexture2D<float2> g_OutputDepth : register(u1, space3);

float3 World(float2 uv, float z)
{
    float4 p = mul(g_InverseViewProjection, float4(uv.x * 2 - 1, 1 - uv.y * 2, z, 1));
    return p.xyz / p.w;
}
void Ray(float2 uv, out float3 origin, out float3 direction)
{
    float3 nearPoint = World(uv, 0);
    origin = g_Camera.w > 0 ? g_Camera.xyz : nearPoint;
    direction = normalize(World(uv, 0.99) - origin);
}
float GeometryDistance(float2 uv, float3 origin, float3 direction)
{
    // Reproject the non-jittered pixel into the actual raster depth grid.
    float4 clip = mul(g_RasterViewProjection, float4(origin + direction, 1));
    float2 depthUv = float2(clip.x, -clip.y) / clip.w * 0.5 + 0.5;
    uint2 pixel = min(uint2(saturate(depthUv) * g_Dimensions.xy), g_Dimensions.xy - 1);
    float z = g_Depth.Load(int3(pixel, 0));
    if (z >= 0.999999) return g_Layer.z;
    // Raster and non-jittered projections share z and w; reconstruct using the
    // original ray's uv to avoid applying jitter twice.
    return max(0, dot(World(uv, z) - origin, direction));
}
float2 Segment(float3 origin, float3 direction)
{
    if (abs(direction.y) < 1e-6)
        return origin.y >= g_Layer.x && origin.y <= g_Layer.x + g_Layer.y
            ? float2(0, g_Layer.z) : float2(0, 0);
    float2 t = (float2(g_Layer.x, g_Layer.x + g_Layer.y) - origin.y) / direction.y;
    return float2(max(0, min(t.x, t.y)), min(g_Layer.z, max(t.x, t.y)));
}
float Density(float3 world, float distance)
{
    float height = (world.y - g_Layer.x) / g_Layer.y;
    if (height <= 0 || height >= 1 || g_Shape.x <= 0 || g_Shape.y <= 0) return 0;
    float3 q = (world - g_Wind.xyz) / g_Layer.w;
    float weather = g_Noise.SampleLevel(g_Wrap, float3(q.x * 0.037, 0.37, q.z * 0.037), 0);
    float base = g_Noise.SampleLevel(g_Wrap, q * 0.125, 0);
    float bump = g_Noise.SampleLevel(g_Wrap, q * 0.25 + 0.31, 0);
    float profile = smoothstep(0, 0.10, height) * (1 - smoothstep(0.45, 1, height));
    float threshold = lerp(0.88, 0.25, g_Shape.x);
    float shape = (base * 0.7 + bump * 0.3 + weather * 0.3) * profile - threshold;
    float detail = g_Noise.SampleLevel(g_Wrap, q * 0.7 + g_Wind.w, 0);
    shape -= (1 - detail) * g_Shape.w * saturate(1 - distance / g_Quality.z);
    return smoothstep(0, lerp(0.24, 0.1, g_Shape.z), shape);
}
void March(uint2 pixel)
{
    float2 uv = (pixel + 0.5) / g_Dimensions.zw;
    float3 origin, direction;
    Ray(uv, origin, direction);
    float2 segment = Segment(origin, direction);
    float geometry = GeometryDistance(uv, origin, direction);
    segment.y = min(segment.y, geometry);
    float4 result = float4(0, 0, 0, 1);
    float representative = g_Layer.z;
    if (segment.y > segment.x && g_Shape.x > 0 && g_Shape.y > 0)
    {
        float stepLength = (segment.y - segment.x) / g_Quality.x;
        // Deterministic interleaved gradient offset, with a finite low-discrepancy
        // sequence. It is not a claim of spatiotemporal blue-noise sampling.
        float offset = frac(52.9829189 * frac(dot(float2(pixel), float2(.06711056, .00583715)))
            + (g_State.y % 16) * 0.618033989);
        float weightedDistance = 0, weight = 0;
        [loop] for (uint i = 0; i < uint(g_Quality.x); ++i)
        {
            float t = segment.x + (i + offset) * stepLength;
            float3 p = origin + direction * t;
            float density = Density(p, t);
            if (density <= 0.001) continue;
            float optical = 0;
            float lightStep = g_Layer.y / max(1, g_Quality.y);
            [loop] for (uint j = 0; j < uint(g_Quality.y); ++j)
                optical += Density(p + g_Sun.xyz * ((j + 0.5) * lightStep), t) * lightStep;
            float light = exp(-optical * g_Shape.y * 0.7);
            float forward = pow(saturate(dot(direction, g_Sun.xyz)), 8) * g_Lit.w;
            float3 radiance = g_Shadow.rgb * g_Shadow.w +
                g_Lit.rgb * g_SunColor.rgb * (light + forward * light);
            float transmission = exp(-g_Shape.y * density * stepLength);
            float contribution = result.a * (1 - transmission);
            result.rgb += contribution * radiance;
            weightedDistance += contribution * t;
            weight += contribution;
            result.a *= transmission;
            if (result.a < 0.015) break;
        }
        if (weight > 0.001) representative = weightedDistance / weight;
    }
    g_Output[pixel] = result;
    g_OutputDepth[pixel] = float2(representative, geometry);
}
void Temporal(uint2 pixel)
{
    float4 current = g_Current.Load(int3(pixel, 0));
    float2 depths = g_CurrentDepth.Load(int3(pixel, 0));
    float2 uv = (pixel + 0.5) / g_Dimensions.zw;
    float3 origin, direction;
    Ray(uv, origin, direction);
    float3 cloudPosition = origin + direction * depths.x - g_Wind.xyz + g_PreviousWind.xyz;
    float4 previous = mul(g_PreviousViewProjection, float4(cloudPosition, 1));
    float2 prevUv = float2(previous.x, -previous.y) / previous.w * .5 + .5;
    if (g_State.z != 0 && previous.w > 0 && all(prevUv > 0) && all(prevUv < 1))
    {
        int2 oldPixel = min(int2(prevUv * g_Dimensions.zw), int2(g_Dimensions.zw) - 1);
        float4 old = g_History.Load(int3(oldPixel, 0));
        float2 oldDepth = g_HistoryDepth.Load(int3(oldPixel, 0));
        float4 low = current, high = current;
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            int2 p = clamp(int2(pixel) + int2(x,y), 0, int2(g_Dimensions.zw) - 1);
            float4 sampleValue = g_Current.Load(int3(p, 0));
            low = min(low, sampleValue); high = max(high, sampleValue);
        }
        float confidence = saturate(1 - abs(old.a - current.a) * 4);
        confidence *= abs(oldDepth.y - depths.y) < max(2, depths.y * .05) ? 1 : 0;
        confidence *= abs(oldDepth.x - depths.x) < max(g_Layer.y, depths.x * .1) ? 1 : 0;
        float motion = length((prevUv - uv) * g_Dimensions.zw);
        confidence *= saturate(1 - motion / 48);
        current = lerp(current, clamp(old, low, high), g_Quality.w * confidence);
    }
    g_Output[pixel] = current;
    g_OutputDepth[pixel] = depths;
}
[numthreads(8, 8, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_Dimensions.zw)) return;
    if (g_State.x == 0) March(id.xy); else Temporal(id.xy);
}
