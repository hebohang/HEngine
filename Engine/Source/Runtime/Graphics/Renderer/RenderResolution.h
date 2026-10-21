#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace HEngine
{
    struct RenderExtent { uint32_t Width; uint32_t Height; };

    // A height cap keeps the display aspect ratio and never supersamples a
    // smaller window. Zero preserves native resolution for existing projects.
    [[nodiscard]] inline RenderExtent ResolveRenderExtent(uint32_t outputWidth,
        uint32_t outputHeight, uint32_t requestedHeight) noexcept
    {
        if (outputWidth == 0 || outputHeight == 0 || requestedHeight == 0 || requestedHeight >= outputHeight)
            return {outputWidth, outputHeight};
        const auto width = static_cast<uint32_t>(std::llround(
            double(outputWidth) * requestedHeight / outputHeight));
        return {std::clamp(width, 1u, outputWidth), requestedHeight};
    }
}
