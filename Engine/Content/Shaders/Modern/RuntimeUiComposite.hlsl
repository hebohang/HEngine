#pragma htype vs VS
#pragma htype ps PS

#include "Shared/BackendPushConstants.hlsli"

struct RuntimeUiConstants
{
    uint PassKind;
    uint Effect;
    uint Flags;
    float Progress;
    float2 InverseTargetSize;
    float EdgeSoftnessPixels;
    float HalftoneCellPixels;
    float2 RevealCenter;
    float2 Padding;
};
HE_DECLARE_PUSH_CONSTANTS(RuntimeUiConstants, g_RuntimeUiConstants, space3);

#define g_PassKind g_RuntimeUiConstants.PassKind
#define g_Effect g_RuntimeUiConstants.Effect
#define g_Flags g_RuntimeUiConstants.Flags
#define g_Progress g_RuntimeUiConstants.Progress
#define g_InverseTargetSize g_RuntimeUiConstants.InverseTargetSize
#define g_EdgeSoftnessPixels g_RuntimeUiConstants.EdgeSoftnessPixels
#define g_HalftoneCellPixels g_RuntimeUiConstants.HalftoneCellPixels
#define g_RevealCenter g_RuntimeUiConstants.RevealCenter

Texture2D<float4> g_UiTexture : register(t0, space3);
SamplerState g_LinearClampSampler : register(s0, space3);

static const uint c_PassComposite = 1u;
static const uint c_EffectNone = 0u;
static const uint c_EffectDiagonalStickerWipe = 1u;
static const uint c_EffectStarPetalReveal = 2u;
static const uint c_EffectHalftoneDotReveal = 3u;
static const uint c_FlagReduceMotion = 1u;
static const uint c_FlagReduceFlash = 2u;
static const float c_Tau = 6.28318530718f;

struct FullscreenVaryings
{
    float4 PositionCS : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

FullscreenVaryings VS(uint vertexId : SV_VertexID)
{
    FullscreenVaryings output;
    const float2 position = float2(
        (vertexId == 2) ? 3.0f : -1.0f,
        (vertexId == 1) ? 3.0f : -1.0f);
    output.PositionCS = float4(position, 0.0f, 1.0f);
    output.TexCoord = float2(position.x * 0.5f + 0.5f, 0.5f - position.y * 0.5f);
    return output;
}

float SafeProgress()
{
    return isfinite(g_Progress) ? saturate(g_Progress) : 1.0f;
}

float SafeSmoothStep(float edge0, float edge1, float value)
{
    return smoothstep(edge0, max(edge1, edge0 + 1e-5f), value);
}

float DiagonalStickerCoverage(float2 uv, float progress, float softness)
{
    // Top-left to bottom-right diagonal, widened enough that progress=1
    // deterministically covers every pixel.
    const float field = saturate((uv.x + uv.y * 0.72f) / 1.72f);
    return SafeSmoothStep(field - softness, field + softness, progress);
}

float StarPetalCoverage(float2 uv, float progress, float softness)
{
    const float2 targetSize = rcp(max(g_InverseTargetSize, 1e-6f));
    const float aspect = targetSize.x / max(targetSize.y, 1.0f);
    float2 delta = uv - saturate(g_RevealCenter);
    delta.x *= aspect;

    const float radius = length(delta);
    const float angle = atan2(delta.y, delta.x);
    // A soft five-petal silhouette, normalized against the furthest corner.
    const float petal = 0.86f + 0.14f * cos(angle * 5.0f - c_Tau * 0.125f);
    const float normalizedDistance = saturate(radius * petal / max(0.72f * max(aspect, 1.0f), 1e-4f));
    return SafeSmoothStep(normalizedDistance - softness, normalizedDistance + softness, progress);
}

float HalftoneCoverage(float2 uv, float progress, float softnessPixels)
{
    const float2 targetSize = rcp(max(g_InverseTargetSize, 1e-6f));
    const float cellPixels = clamp(g_HalftoneCellPixels, 4.0f, 128.0f);
    const float2 gridSize = max(targetSize / cellPixels, 1.0f);
    const float2 local = frac(uv * gridSize) - 0.5f;
    const float2 centerDelta = uv - saturate(g_RevealCenter);
    const float radialDelay = saturate(length(centerDelta) * 0.32f);
    const float localProgress = saturate(progress * 1.32f - radialDelay);
    const float radius = localProgress * 0.76f;
    const float softness = max(softnessPixels / cellPixels, 0.002f);
    return 1.0f - SafeSmoothStep(radius - softness, radius + softness, length(local));
}

float TransitionCoverage(float2 uv)
{
    const float progress = SafeProgress();
    if (progress <= 0.0f)
        return 0.0f;
    if (progress >= 1.0f)
        return 1.0f;

    // Both accessibility switches avoid spatial motion and flashing. The
    // replacement is a monotonic, low-frequency opacity fade.
    if ((g_Flags & (c_FlagReduceMotion | c_FlagReduceFlash)) != 0u)
        return smoothstep(0.0f, 1.0f, progress);

    if (g_Effect == c_EffectDiagonalStickerWipe)
    {
        const float softness = max(
            g_EdgeSoftnessPixels * max(g_InverseTargetSize.x, g_InverseTargetSize.y),
            1e-4f);
        return DiagonalStickerCoverage(uv, progress, softness);
    }
    if (g_Effect == c_EffectStarPetalReveal)
    {
        const float softness = max(
            g_EdgeSoftnessPixels * max(g_InverseTargetSize.x, g_InverseTargetSize.y),
            1e-4f);
        return StarPetalCoverage(uv, progress, softness);
    }
    if (g_Effect == c_EffectHalftoneDotReveal)
        return HalftoneCoverage(uv, progress, g_EdgeSoftnessPixels);
    if (g_Effect == c_EffectNone)
        return 1.0f;
    return 1.0f;
}

float4 SanitizePremultiplied(float4 color)
{
    if (!all(isfinite(color)))
        return 0.0f;
    const float alpha = saturate(color.a);
    return float4(clamp(color.rgb, 0.0f, alpha), alpha);
}

float4 PS(FullscreenVaryings input) : SV_Target0
{
    float4 ui = SanitizePremultiplied(
        g_UiTexture.Sample(g_LinearClampSampler, input.TexCoord));
    if (g_PassKind == c_PassComposite)
        return ui;

    // Apply coverage to RGB and alpha together to preserve premultiplication.
    return ui * TransitionCoverage(input.TexCoord);
}
