#pragma htype cs CS

#include "Shared/BackendPushConstants.hlsli"

struct PostProcessConstants
{
    uint2 SourceDimensions;
    uint2 OutputDimensions;
    uint Operation;
    uint AutoExposure;
    uint ResetHistory;
    uint EdgeFootprint; // native pixel count on the last quarter-resolution column/row
    float4 BloomExposure; // intensity, scene-linear threshold, manual exposure, middle gray
    float4 Adaptation;   // minimum exposure, maximum exposure, speed / second, real delta seconds
    float4 Color;        // contrast, saturation, artistic warm/cool offset, vignette
    float4 ColorGain;    // exposed-linear RGB gain; w unused
    float4 ColorOffset;  // exposed-linear RGB offset; w unused
    float4 LocalExposure; // shadow strength, highlight strength, max stops, player exposure compensation EV
};
HE_DECLARE_PUSH_CONSTANTS(PostProcessConstants, g_PostProcess, space3);

Texture2D<float4> g_Input : register(t0, space3);
Texture2D<float4> g_Bloom : register(t1, space3);
Texture2D<float4> g_Exposure : register(t2, space3);
RWTexture2D<float4> g_Output : register(u0, space3);
SamplerState g_LinearClamp : register(s0, space3);

groupshared float2 g_Reduction[64];

float3 SafeHdr(float3 color)
{
    return all(isfinite(color)) ? clamp(color, 0.0f, 65504.0f) : 0.0f;
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

void ReduceGroup(uint groupIndex)
{
    GroupMemoryBarrierWithGroupSync();
    [unroll]
    for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (groupIndex < stride)
            g_Reduction[groupIndex] += g_Reduction[groupIndex + stride];
        GroupMemoryBarrierWithGroupSync();
    }
}

[numthreads(8, 8, 1)]
void CS(uint3 dispatchId : SV_DispatchThreadID, uint3 groupId : SV_GroupID,
    uint groupIndex : SV_GroupIndex)
{
    // Exposure uses quarter-resolution luminance, two GPU reductions and one
    // persistent scalar. No CPU mapping, readback, histogram clipping or fixed
    // per-frame adaptation is hidden in this path.
    if (g_PostProcess.Operation == 3u)
    {
        const bool valid = all(dispatchId.xy < g_PostProcess.SourceDimensions);
        const uint columnPixels = dispatchId.x + 1u == g_PostProcess.SourceDimensions.x
            ? g_PostProcess.EdgeFootprint & 255u : 4u;
        const uint rowPixels = dispatchId.y + 1u == g_PostProcess.SourceDimensions.y
            ? (g_PostProcess.EdgeFootprint >> 8u) & 255u : 4u;
        const float weight = float(columnPixels * rowPixels);
        g_Reduction[groupIndex] = valid
            ? float2(g_Input.Load(int3(dispatchId.xy, 0)).a * weight, weight) : 0.0f;
        ReduceGroup(groupIndex);
        if (groupIndex == 0u)
            g_Output[groupId.xy] = float4(g_Reduction[0], 0.0f, 0.0f);
        return;
    }
    if (g_PostProcess.Operation == 4u)
    {
        float2 sum = 0.0f;
        const uint tileCount = g_PostProcess.SourceDimensions.x * g_PostProcess.SourceDimensions.y;
        for (uint tile = groupIndex; tile < tileCount; tile += 64u)
        {
            const uint2 pixel = uint2(tile % g_PostProcess.SourceDimensions.x,
                tile / g_PostProcess.SourceDimensions.x);
            sum += g_Input.Load(int3(pixel, 0)).xy;
        }
        g_Reduction[groupIndex] = sum;
        ReduceGroup(groupIndex);
        if (groupIndex == 0u)
        {
            const float averageLogLuminance = g_Reduction[0].x / max(g_Reduction[0].y, 1.0f);
            const float target = clamp(g_PostProcess.BloomExposure.w /
                max(exp2(averageLogLuminance), 0.0001f),
                g_PostProcess.Adaptation.x, g_PostProcess.Adaptation.y);
            float adapted = target;
            if (g_PostProcess.ResetHistory == 0u)
            {
                const float previous = g_Output[uint2(0, 0)].x;
                const float weight = 1.0f - exp(-g_PostProcess.Adaptation.z * g_PostProcess.Adaptation.w);
                adapted = isfinite(previous) && previous > 0.0f
                    ? exp2(lerp(log2(previous), log2(target), weight)) : target;
            }
            g_Output[uint2(0, 0)] = float4(adapted, target, averageLogLuminance, 1.0f);
        }
        return;
    }

    if (any(dispatchId.xy >= g_PostProcess.OutputDimensions))
        return;
    const uint2 pixel = dispatchId.xy;

    if (g_PostProcess.Operation == 0u)
    {
        // Four bilinear samples average a native 4x4 footprint. Bloom and
        // luminance therefore both operate at ceil(native dimensions / 4).
        const float2 sourceBase = float2(pixel * 4u);
        const float2 sourceSize = float2(g_PostProcess.SourceDimensions);
        float3 color = SafeHdr(g_Input.SampleLevel(g_LinearClamp,
            (sourceBase + float2(1, 1)) / sourceSize, 0).rgb);
        color += SafeHdr(g_Input.SampleLevel(g_LinearClamp,
            (sourceBase + float2(3, 1)) / sourceSize, 0).rgb);
        color += SafeHdr(g_Input.SampleLevel(g_LinearClamp,
            (sourceBase + float2(1, 3)) / sourceSize, 0).rgb);
        color += SafeHdr(g_Input.SampleLevel(g_LinearClamp,
            (sourceBase + float2(3, 3)) / sourceSize, 0).rgb);
        color *= 0.25f;
        // A partial final footprint must not over-count clamped edge pixels.
        // Only these boundary tiles need individual native texel loads.
        if (any(pixel * 4u + 4u > g_PostProcess.SourceDimensions))
        {
            color = 0.0f;
            uint count = 0u;
            const uint2 begin = pixel * 4u;
            const uint2 end = min(begin + 4u, g_PostProcess.SourceDimensions);
            for (uint sourceY = begin.y; sourceY < end.y; ++sourceY)
            {
                for (uint sourceX = begin.x; sourceX < end.x; ++sourceX)
                {
                    color += SafeHdr(g_Input.Load(int3(uint2(sourceX, sourceY), 0)).rgb);
                    ++count;
                }
            }
            color /= max(float(count), 1.0f);
        }
        const float brightness = max(max(color.r, color.g), color.b);
        const float threshold = g_PostProcess.BloomExposure.y;
        const float knee = max(threshold * 0.5f, 0.0001f);
        float soft = clamp(brightness - threshold + knee, 0.0f, 2.0f * knee);
        soft = soft * soft / (4.0f * knee + 0.0001f);
        const float contribution = max(brightness - threshold, soft) / max(brightness, 0.0001f);
        g_Output[pixel] = float4(color * max(contribution, 0.0f),
            log2(max(Luminance(color), 0.0001f)));
        return;
    }

    if (g_PostProcess.Operation == 1u || g_PostProcess.Operation == 2u)
    {
        static const float weights[5] = { 0.227027027f, 0.194594595f,
            0.121621622f, 0.054054054f, 0.016216216f };
        const int2 axis = g_PostProcess.Operation == 1u ? int2(1, 0) : int2(0, 1);
        const int2 maximum = int2(g_PostProcess.SourceDimensions) - 1;
        float3 color = g_Input.Load(int3(pixel, 0)).rgb * weights[0];
        const float centerLog = g_Input.Load(int3(pixel, 0)).a;
        float baseLog = centerLog * weights[0];
        float baseWeight = weights[0];
        [unroll]
        for (int offset = 1; offset <= 4; ++offset)
        {
            color += g_Input.Load(int3(clamp(int2(pixel) + axis * offset, int2(0, 0), maximum), 0)).rgb * weights[offset];
            color += g_Input.Load(int3(clamp(int2(pixel) - axis * offset, int2(0, 0), maximum), 0)).rgb * weights[offset];
            if (any(g_PostProcess.LocalExposure.xy > 0.0f) && g_PostProcess.LocalExposure.z > 0.0f)
            {
                // Wider base layer than bloom; bilateral weights prevent bright sky
                // leaking across silhouettes. RGB bloom retains its original kernel.
                [unroll]
                for (int side = -1; side <= 1; side += 2)
                {
                    const float sampleLog = g_Input.Load(int3(clamp(int2(pixel) +
                        axis * offset * side * 4, int2(0, 0), maximum), 0)).a;
                    const float delta = sampleLog - centerLog;
                    const float weight = weights[offset] * exp2(-delta * delta * 0.5f);
                    baseLog += sampleLog * weight;
                    baseWeight += weight;
                }
            }
        }
        g_Output[pixel] = float4(SafeHdr(color), baseLog / baseWeight);
        return;
    }

    float3 sceneColor = SafeHdr(g_Input.Load(int3(pixel, 0)).rgb);
    const float sourceLog = log2(max(Luminance(sceneColor), 0.0001f));
    if (g_PostProcess.BloomExposure.x > 0.0f)
    {
        const float2 uv = (float2(pixel) + 0.5f) / float2(g_PostProcess.OutputDimensions);
        sceneColor += SafeHdr(g_Bloom.SampleLevel(g_LinearClamp, uv, 0).rgb) * g_PostProcess.BloomExposure.x;
    }
    const float manualExposure = g_PostProcess.BloomExposure.z;
    const float exposure = (g_PostProcess.AutoExposure != 0u
        ? g_Exposure.Load(int3(0, 0, 0)).x : manualExposure) * exp2(g_PostProcess.LocalExposure.w);
    float3 exposedColor = SafeHdr(sceneColor * exposure);
    if (any(g_PostProcess.LocalExposure.xy > 0.0f) && g_PostProcess.LocalExposure.z > 0.0f)
    {
        // Joint bilateral upsample of the base layer; retain full-resolution detail
        // and apply one scalar to RGB, preserving hue. No temporal history to trail.
        const uint2 baseSize = (g_PostProcess.OutputDimensions + 3u) / 4u;
        const float2 basePosition = (float2(pixel) + 0.5f) / 4.0f - 0.5f;
        const int2 basePixel = int2(floor(basePosition));
        const float2 fraction = frac(basePosition);
        float baseSum = 0.0f;
        float weightSum = 0.0f;
        [unroll]
        for (int y = 0; y < 2; ++y)
        {
            [unroll]
            for (int x = 0; x < 2; ++x)
            {
                const float base = g_Bloom.Load(int3(clamp(basePixel + int2(x,y),
                    int2(0,0), int2(baseSize) - 1), 0)).a;
                const float delta = base - sourceLog;
                const float spatial = (x == 0 ? 1.0f-fraction.x : fraction.x) *
                    (y == 0 ? 1.0f-fraction.y : fraction.y);
                const float weight = spatial * exp2(-delta * delta * 0.5f);
                baseSum += base * weight;
                weightSum += weight;
            }
        }
        const float baseLog = weightSum > 0.0001f ? baseSum / weightSum : sourceLog;
        const float deviation = log2(0.18f) - (baseLog + log2(max(exposure, 1e-10f)));
        const float strength = deviation > 0.0f ? g_PostProcess.LocalExposure.x : g_PostProcess.LocalExposure.y;
        const float adjustment = clamp(deviation * strength,
            -g_PostProcess.LocalExposure.z, g_PostProcess.LocalExposure.z);
        exposedColor *= exp2(adjustment);
    }
    const float temperature = g_PostProcess.Color.z;
    exposedColor *= exp2(float3(temperature * 0.5f, 0.0f, -temperature * 0.5f));
    exposedColor = lerp(Luminance(exposedColor).xxx, exposedColor, g_PostProcess.Color.y);
    exposedColor = 0.18f * pow(max(exposedColor / 0.18f, 0.0f), g_PostProcess.Color.x);
    exposedColor = max(exposedColor * g_PostProcess.ColorGain.rgb +
        g_PostProcess.ColorOffset.rgb, 0.0f);
    const float2 uv = (float2(pixel) + 0.5f) / float2(g_PostProcess.OutputDimensions);
    // Keep the center and mid-frame edges readable; darken the corners first.
    const float2 centered = (uv - 0.5f) * 2.0f;
    const float edge = smoothstep(0.75f, 1.4f, length(centered));
    exposedColor *= 1.0f - saturate(g_PostProcess.Color.w) * edge;
    // SceneToneMap remains the only final exposure + ACES + sRGB operation.
    // Undo its manual multiplier here, including in auto mode. Explicit manual
    // exposure zero remains black and never produces a division-by-zero NaN.
    g_Output[pixel] = float4(manualExposure > 0.0f
        ? SafeHdr(exposedColor / manualExposure) : float3(0, 0, 0), 1.0f);
}
