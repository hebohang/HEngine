#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace HEngine
{
    struct TextureAlphaDilationResult
    {
        // Empty when no RGB change was required. Callers can retain the source
        // payload in that case instead of paying for a redundant full copy.
        std::vector<std::byte> Pixels;
        std::uint32_t DilatedPixelCount = 0;

        [[nodiscard]] bool WasApplied() const noexcept
        {
            return DilatedPixelCount != 0;
        }
    };

    // Copies RGB from the nearest alpha-tested texel into rejected RGBA8
    // texels while preserving every original alpha value. This prevents black
    // transparent padding from bleeding into mask edges and generated mips.
    [[nodiscard]] TextureAlphaDilationResult DilateMaskedTextureRgb(
        std::span<const std::byte> rgbaPixels,
        std::uint32_t width,
        std::uint32_t height,
        float alphaCutoff);
}
