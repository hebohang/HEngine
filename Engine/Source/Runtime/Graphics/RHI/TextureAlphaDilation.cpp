#include "Hepch.h"

#include "Runtime/Graphics/RHI/TextureAlphaDilation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace HEngine
{
    TextureAlphaDilationResult DilateMaskedTextureRgb(
        std::span<const std::byte> rgbaPixels,
        std::uint32_t width,
        std::uint32_t height,
        float alphaCutoff)
    {
        if (width == 0 || height == 0 || !std::isfinite(alphaCutoff))
            throw std::invalid_argument(
                "Texture alpha dilation requires valid dimensions and cutoff.");
        const std::uint64_t pixelCount64 =
            static_cast<std::uint64_t>(width) * height;
        if (pixelCount64 > std::numeric_limits<std::size_t>::max() / 4u ||
            pixelCount64 > std::numeric_limits<std::uint32_t>::max() ||
            rgbaPixels.size() != static_cast<std::size_t>(pixelCount64) * 4u)
        {
            throw std::invalid_argument(
                "Texture alpha dilation RGBA8 payload size is invalid.");
        }

        const std::size_t pixelCount = static_cast<std::size_t>(pixelCount64);
        const std::uint8_t cutoff = static_cast<std::uint8_t>(std::clamp<float>(
            std::ceil(std::clamp(alphaCutoff, 0.0f, 1.0f) * 255.0f),
            0.0f,
            255.0f));

        std::uint32_t opaquePixelCount = 0;
        for (std::size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex)
        {
            const std::uint8_t alpha = std::to_integer<std::uint8_t>(
                rgbaPixels[pixelIndex * 4u + 3u]);
            opaquePixelCount += alpha >= cutoff ? 1u : 0u;
        }
        if (opaquePixelCount == 0u || opaquePixelCount == pixelCount)
            return {};

        TextureAlphaDilationResult result{
            .Pixels = std::vector<std::byte>(
                rgbaPixels.begin(),
                rgbaPixels.end()),
        };
        std::vector<std::uint8_t> filled(pixelCount, 0u);
        std::vector<std::uint32_t> queue;
        queue.reserve(pixelCount);
        for (std::uint32_t pixelIndex = 0;
            pixelIndex < static_cast<std::uint32_t>(pixelCount);
            ++pixelIndex)
        {
            const std::uint8_t alpha = std::to_integer<std::uint8_t>(
                rgbaPixels[static_cast<std::size_t>(pixelIndex) * 4u + 3u]);
            if (alpha < cutoff)
                continue;
            filled[pixelIndex] = 1u;
            queue.push_back(pixelIndex);
        }

        const auto visitNeighbors = [width, height](
            std::size_t pixelIndex,
            auto&& visitor)
        {
            const std::uint32_t x =
                static_cast<std::uint32_t>(pixelIndex % width);
            const std::uint32_t y =
                static_cast<std::uint32_t>(pixelIndex / width);
            if (x > 0)
                visitor(pixelIndex - 1u);
            if (x + 1u < width)
                visitor(pixelIndex + 1u);
            if (y > 0)
                visitor(pixelIndex - width);
            if (y + 1u < height)
                visitor(pixelIndex + width);
        };

        for (std::size_t readIndex = 0; readIndex < queue.size(); ++readIndex)
        {
            const std::size_t sourceIndex = queue[readIndex];
            visitNeighbors(sourceIndex, [&](std::size_t neighbor)
            {
                if (filled[neighbor] != 0u)
                    return;
                const std::size_t sourceOffset = sourceIndex * 4u;
                const std::size_t destinationOffset = neighbor * 4u;
                std::copy_n(
                    result.Pixels.data() + sourceOffset,
                    3u,
                    result.Pixels.data() + destinationOffset);
                // Alpha is intentionally left unchanged.
                filled[neighbor] = 1u;
                queue.push_back(static_cast<std::uint32_t>(neighbor));
                ++result.DilatedPixelCount;
            });
        }
        return result;
    }
}
