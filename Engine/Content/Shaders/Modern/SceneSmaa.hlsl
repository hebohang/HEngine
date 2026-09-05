#pragma htype vs VS
#pragma htype ps PS
#pragma hvariant BLEND_WEIGHT
#pragma hvariant NEIGHBORHOOD

#include "Shared/BackendPushConstants.hlsli"

struct SmaaConstants
{
    uint2 Dimensions;
    float EdgeThreshold;
    float LocalContrastAdaptation;
    uint MaxSearchSteps;
    uint MaxDiagonalSearchSteps;
    float CornerRounding;
    float Padding;
};
HE_DECLARE_PUSH_CONSTANTS(SmaaConstants, g_SmaaConstants, space3);

// BASE: Source0 is display-referred color and Source1 aliases Source0.
// BLEND_WEIGHT: both sources alias the edge texture.
// NEIGHBORHOOD: Source0 is color and Source1 is the blend-weight texture.
Texture2D<float4> g_Source0 : register(t0, space3);
Texture2D<float4> g_Source1 : register(t1, space3);
SamplerState g_LinearClampSampler : register(s0, space3);

struct FullscreenVaryings
{
    float4 PositionCS : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

FullscreenVaryings VS(uint vertexId : SV_VertexID)
{
    const float2 position = float2(
        vertexId == 2u ? 3.0f : -1.0f,
        vertexId == 1u ? 3.0f : -1.0f);
    FullscreenVaryings output;
    output.PositionCS = float4(position, 0.0f, 1.0f);
    output.TexCoord = float2(
        position.x * 0.5f + 0.5f,
        0.5f - position.y * 0.5f);
    return output;
}

uint2 Dimensions()
{
    return max(g_SmaaConstants.Dimensions, 1u);
}

bool IsInside(int2 pixel)
{
    return all(pixel >= 0) && all(pixel < int2(Dimensions()));
}

int2 ClampPixel(int2 pixel)
{
    return clamp(pixel, int2(0, 0), int2(Dimensions()) - 1);
}

float4 LoadSource0(int2 pixel)
{
    return g_Source0.Load(int3(ClampPixel(pixel), 0));
}

float2 LoadEdges(int2 pixel)
{
    if (!IsInside(pixel))
        return 0.0f;
    const float2 edges = g_Source0.Load(int3(pixel, 0)).rg;
    return all(isfinite(edges)) ? saturate(edges) : 0.0f;
}

float LoadEdge(int2 pixel, uint component)
{
    const float2 edges = LoadEdges(pixel);
    return component == 0u ? edges.x : edges.y;
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

#if BLEND_WEIGHT

uint SearchEdge(
    int2 pixel,
    int2 direction,
    uint component,
    uint maximumSteps)
{
    uint distance = 0u;
    [loop]
    for (uint stepIndex = 1u; stepIndex <= 32u; ++stepIndex)
    {
        if (stepIndex > maximumSteps)
            break;
        const int2 samplePixel = pixel + direction * int(stepIndex);
        if (!IsInside(samplePixel) || LoadEdge(samplePixel, component) < 0.5f)
            break;
        distance = stepIndex;
    }
    return distance;
}

float2 IntegrateSignedLine(float y0, float y1)
{
    if (y0 >= 0.0f && y1 >= 0.0f)
        return float2(0.0f, 0.5f * (y0 + y1));
    if (y0 <= 0.0f && y1 <= 0.0f)
        return float2(-0.5f * (y0 + y1), 0.0f);

    const float denominator = max(abs(y0) + abs(y1), 1e-6f);
    const float positive = y0 > 0.0f ? y0 : y1;
    const float negative = y0 < 0.0f ? -y0 : -y1;
    return float2(
        0.5f * negative * negative / denominator,
        0.5f * positive * positive / denominator);
}

// Analytic equivalent of the orthogonal SMAA area lookup. It integrates the
// two signed triangle/trapezoid areas covered by a revectorized line segment
// over one pixel, so no external AreaTex quantization or lifetime is needed.
float2 IntegrateLineSegment(float2 p0, float2 p1, float pixelPosition)
{
    const float x0 = max(pixelPosition, p0.x);
    const float x1 = min(pixelPosition + 1.0f, p1.x);
    if (x1 <= x0 || p1.x <= p0.x)
        return 0.0f;

    const float inverseLength = rcp(p1.x - p0.x);
    const float y0 = lerp(p0.y, p1.y, (x0 - p0.x) * inverseLength);
    const float y1 = lerp(p0.y, p1.y, (x1 - p0.x) * inverseLength);
    return IntegrateSignedLine(y0, y1) * (x1 - x0);
}

float EndpointOffset(float positiveTurn, float negativeTurn)
{
    // A crossing in both directions is a T junction, not a revectorizable
    // endpoint. Treating it as zero is the conservative SMAA corner rule.
    if (positiveTurn >= 0.5f && negativeTurn >= 0.5f)
        return 0.0f;
    return 0.5f * (positiveTurn - negativeTurn);
}

float2 EvaluateOrthogonalArea(
    uint negativeDistance,
    uint positiveDistance,
    float negativeEndpoint,
    float positiveEndpoint)
{
    const float span = float(negativeDistance + positiveDistance + 1u);
    const float pixelPosition = float(negativeDistance);
    const float midpoint = 0.5f * span;
    const bool bHasNegativeEndpoint = abs(negativeEndpoint) > 1e-5f;
    const bool bHasPositiveEndpoint = abs(positiveEndpoint) > 1e-5f;

    if (!bHasNegativeEndpoint && !bHasPositiveEndpoint)
        return 0.0f;

    float2 area = 0.0f;
    if (bHasNegativeEndpoint && bHasPositiveEndpoint &&
        negativeEndpoint * positiveEndpoint < 0.0f)
    {
        // Z pattern: one continuous line connects opposite endpoint turns.
        area = IntegrateLineSegment(
            float2(0.0f, negativeEndpoint),
            float2(span, positiveEndpoint),
            pixelPosition);
    }
    else
    {
        // L and U patterns converge on the center. Keeping the other half
        // unfiltered is what prevents straight axis-aligned edges from blur.
        if (bHasNegativeEndpoint)
        {
            area += IntegrateLineSegment(
                float2(0.0f, negativeEndpoint),
                float2(midpoint, 0.0f),
                pixelPosition);
        }
        if (bHasPositiveEndpoint)
        {
            area += IntegrateLineSegment(
                float2(midpoint, 0.0f),
                float2(span, positiveEndpoint),
                pixelPosition);
        }
    }
    return min(area, 0.5f.xx);
}

uint SearchVerticalDiagonal(
    int2 pixel,
    int2 direction,
    uint maximumSteps)
{
    uint distance = 0u;
    int2 current = pixel;
    [loop]
    for (uint stepIndex = 1u; stepIndex <= 16u; ++stepIndex)
    {
        if (stepIndex > maximumSteps)
            break;

        int2 connector = current;
        if (direction.x < 0)
            connector.x -= 1;
        if (direction.y > 0)
            connector.y += 1;
        const int2 next = current + direction;
        if (!IsInside(connector) || !IsInside(next) ||
            LoadEdge(connector, 1u) < 0.5f ||
            LoadEdge(next, 0u) < 0.5f)
            break;

        distance = stepIndex;
        current = next;
    }
    return distance;
}

uint SearchHorizontalDiagonal(
    int2 pixel,
    int2 direction,
    uint maximumSteps)
{
    uint distance = 0u;
    int2 current = pixel;
    [loop]
    for (uint stepIndex = 1u; stepIndex <= 16u; ++stepIndex)
    {
        if (stepIndex > maximumSteps)
            break;

        int2 connector = current;
        if (direction.x > 0)
            connector.x += 1;
        if (direction.y < 0)
            connector.y -= 1;
        const int2 next = current + direction;
        if (!IsInside(connector) || !IsInside(next) ||
            LoadEdge(connector, 0u) < 0.5f ||
            LoadEdge(next, 1u) < 0.5f)
            break;

        distance = stepIndex;
        current = next;
    }
    return distance;
}

float DiagonalAreaFloor(uint diagonalLength)
{
    // A 45-degree line crossing a unit pixel forms two 1/8-pixel triangles.
    // Exact alternating edge topology is required by the diagonal searches;
    // unrelated neighboring edges can never activate this path.
    return diagonalLength > 0u ? 0.125f : 0.0f;
}

float CornerAttenuation(
    uint negativeDistance,
    uint positiveDistance,
    float negativeTurnStrength,
    float positiveTurnStrength)
{
    const float nearestEndpoint = float(min(
        negativeTurnStrength > 0.0f ? negativeDistance : 4u,
        positiveTurnStrength > 0.0f ? positiveDistance : 4u));
    const float endpointProximity = 1.0f - saturate(
        (nearestEndpoint + 1.0f) * 0.25f);
    const float turnStrength = saturate(max(
        negativeTurnStrength,
        positiveTurnStrength));
    return 1.0f - saturate(g_SmaaConstants.CornerRounding) *
        endpointProximity * turnStrength;
}

float4 PS(FullscreenVaryings input) : SV_Target0
{
    const int2 pixel = min(
        int2(input.PositionCS.xy),
        int2(Dimensions()) - 1);
    const float2 edges = LoadEdges(pixel);
    const uint maximumSteps = clamp(
        g_SmaaConstants.MaxSearchSteps,
        1u,
        32u);
    const uint maximumDiagonalSteps = min(
        g_SmaaConstants.MaxDiagonalSearchSteps,
        16u);

    // RGBA stores four directional weights for the edge owned by this pixel:
    // R=current->left, G=current->top, B=left->current, A=top->current.
    float4 weights = 0.0f;
    if (edges.x >= 0.5f)
    {
        const uint up = SearchEdge(pixel, int2(0, -1), 0u, maximumSteps);
        const uint down = SearchEdge(pixel, int2(0, 1), 0u, maximumSteps);
        const int2 topEndpoint = pixel + int2(0, -int(up));
        const int2 bottomEndpoint = pixel + int2(0, int(down) + 1);
        const float topRight = LoadEdge(topEndpoint, 1u);
        const float topLeft = LoadEdge(topEndpoint + int2(-1, 0), 1u);
        const float bottomRight = LoadEdge(bottomEndpoint, 1u);
        const float bottomLeft = LoadEdge(bottomEndpoint + int2(-1, 0), 1u);
        float2 area = EvaluateOrthogonalArea(
            up,
            down,
            EndpointOffset(topRight, topLeft),
            EndpointOffset(bottomRight, bottomLeft));

        const uint diagonalA = SearchVerticalDiagonal(
            pixel, int2(1, 1), maximumDiagonalSteps) +
            SearchVerticalDiagonal(
                pixel, int2(-1, -1), maximumDiagonalSteps);
        const uint diagonalB = SearchVerticalDiagonal(
            pixel, int2(-1, 1), maximumDiagonalSteps) +
            SearchVerticalDiagonal(
                pixel, int2(1, -1), maximumDiagonalSteps);
        area = max(area, DiagonalAreaFloor(max(diagonalA, diagonalB)).xx);
        // A luminance edge can remain an unresolved subpixel boundary even
        // when its finite segment has no crossing inside the search window.
        // Preserve a conservative coverage floor instead of silently turning
        // such long axis-aligned or clipped edges into a no-op.
        area = max(area, 0.125f.xx);
        area *= CornerAttenuation(
            up,
            down,
            saturate(topRight + topLeft),
            saturate(bottomRight + bottomLeft));
        weights.r = area.y;
        weights.b = area.x;
    }

    if (edges.y >= 0.5f)
    {
        const uint left = SearchEdge(pixel, int2(-1, 0), 1u, maximumSteps);
        const uint right = SearchEdge(pixel, int2(1, 0), 1u, maximumSteps);
        const int2 leftEndpoint = pixel + int2(-int(left), 0);
        const int2 rightEndpoint = pixel + int2(int(right) + 1, 0);
        const float leftDown = LoadEdge(leftEndpoint, 0u);
        const float leftUp = LoadEdge(leftEndpoint + int2(0, -1), 0u);
        const float rightDown = LoadEdge(rightEndpoint, 0u);
        const float rightUp = LoadEdge(rightEndpoint + int2(0, -1), 0u);
        float2 area = EvaluateOrthogonalArea(
            left,
            right,
            EndpointOffset(leftDown, leftUp),
            EndpointOffset(rightDown, rightUp));

        const uint diagonalA = SearchHorizontalDiagonal(
            pixel, int2(1, 1), maximumDiagonalSteps) +
            SearchHorizontalDiagonal(
                pixel, int2(-1, -1), maximumDiagonalSteps);
        const uint diagonalB = SearchHorizontalDiagonal(
            pixel, int2(1, -1), maximumDiagonalSteps) +
            SearchHorizontalDiagonal(
                pixel, int2(-1, 1), maximumDiagonalSteps);
        area = max(area, DiagonalAreaFloor(max(diagonalA, diagonalB)).xx);
        area = max(area, 0.125f.xx);
        area *= CornerAttenuation(
            left,
            right,
            saturate(leftDown + leftUp),
            saturate(rightDown + rightUp));
        weights.g = area.y;
        weights.a = area.x;
    }

    return saturate(weights);
}

#elif NEIGHBORHOOD

float4 LoadWeights(int2 pixel)
{
    if (!IsInside(pixel))
        return 0.0f;
    const float4 weights = g_Source1.Load(int3(pixel, 0));
    return all(isfinite(weights)) ? saturate(weights) : 0.0f;
}

float4 PS(FullscreenVaryings input) : SV_Target0
{
    const int2 pixel = min(
        int2(input.PositionCS.xy),
        int2(Dimensions()) - 1);
    const float4 center = LoadSource0(pixel);
    if (!all(isfinite(center)))
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    const float4 ownWeights = LoadWeights(pixel);
    const float leftWeight = ownWeights.r;
    const float topWeight = ownWeights.g;
    const float rightWeight = LoadWeights(pixel + int2(1, 0)).b;
    const float bottomWeight = LoadWeights(pixel + int2(0, 1)).a;
    const float horizontalWeight = max(leftWeight, rightWeight);
    const float verticalWeight = max(topWeight, bottomWeight);
    const float2 texelSize = rcp(float2(Dimensions()));
    const float2 centerUv = (float2(pixel) + 0.5f) * texelSize;

    float4 result = center;
    if (horizontalWeight >= verticalWeight && horizontalWeight > 1e-5f)
    {
        const float normalization = max(leftWeight + rightWeight, 1e-6f);
        const float2 pairWeight =
            float2(leftWeight, rightWeight) / normalization;
        const float4 leftSample = g_Source0.SampleLevel(
            g_LinearClampSampler,
            centerUv - float2(leftWeight * texelSize.x, 0.0f),
            0.0f);
        const float4 rightSample = g_Source0.SampleLevel(
            g_LinearClampSampler,
            centerUv + float2(rightWeight * texelSize.x, 0.0f),
            0.0f);
        result = leftSample * pairWeight.x + rightSample * pairWeight.y;
    }
    else if (verticalWeight > 1e-5f)
    {
        const float normalization = max(topWeight + bottomWeight, 1e-6f);
        const float2 pairWeight =
            float2(topWeight, bottomWeight) / normalization;
        const float4 topSample = g_Source0.SampleLevel(
            g_LinearClampSampler,
            centerUv - float2(0.0f, topWeight * texelSize.y),
            0.0f);
        const float4 bottomSample = g_Source0.SampleLevel(
            g_LinearClampSampler,
            centerUv + float2(0.0f, bottomWeight * texelSize.y),
            0.0f);
        result = topSample * pairWeight.x + bottomSample * pairWeight.y;
    }
    if (!all(isfinite(result)))
        result = center;
    result.a = center.a;
    return saturate(result);
}

#else

float4 PS(FullscreenVaryings input) : SV_Target0
{
    const int2 pixel = min(
        int2(input.PositionCS.xy),
        int2(Dimensions()) - 1);
    const float center = Luminance(LoadSource0(pixel).rgb);
    const float left = Luminance(LoadSource0(pixel + int2(-1, 0)).rgb);
    const float top = Luminance(LoadSource0(pixel + int2(0, -1)).rgb);
    const float right = Luminance(LoadSource0(pixel + int2(1, 0)).rgb);
    const float bottom = Luminance(LoadSource0(pixel + int2(0, 1)).rgb);
    const float leftLeft = Luminance(LoadSource0(pixel + int2(-2, 0)).rgb);
    const float topTop = Luminance(LoadSource0(pixel + int2(0, -2)).rgb);
    if (!all(isfinite(float4(center, left, top, right))) ||
        !all(isfinite(float4(bottom, leftLeft, topTop, 0.0f))))
    {
        return 0.0f;
    }

    const float2 delta = abs(center.xx - float2(left, top));
    const float neighborhoodMaximum = max(
        max(delta.x, delta.y),
        max(
            max(abs(center - right), abs(center - bottom)),
            max(abs(left - leftLeft), abs(top - topTop))));
    float2 edges = step(
        max(g_SmaaConstants.EdgeThreshold, 1e-4f).xx,
        delta);
    const float adaptation = max(
        g_SmaaConstants.LocalContrastAdaptation,
        1.0f);
    edges *= step(neighborhoodMaximum.xx / adaptation, delta);
    return float4(edges, 0.0f, 1.0f);
}

#endif
