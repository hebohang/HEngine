#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <span>

namespace HEngine
{
    // Immutable after publication. CDF intervals themselves define the GPU PDF;
    // no separate, unquantized probability table is permitted.
    struct EnvironmentSamplingProposal
    {
        static constexpr uint32_t Width = 128, Height = 64, Cells = Width * Height;
        std::array<float, Cells + 1> Cdf{};

        static std::shared_ptr<const EnvironmentSamplingProposal> Build(
            std::span<const float> rgba, uint32_t width, uint32_t height)
        {
            if (!width || !height || rgba.size() != size_t(width) * height * 4) return {};
            constexpr double pi = 3.14159265358979323846;
            std::array<double, Cells> mass{};
            for (uint32_t y = 0; y < height; ++y)
            {
                const double solidAngle = (2.0 * pi / width) *
                    (std::cos(pi * y / height) - std::cos(pi * (y + 1.0) / height));
                const uint32_t row = std::min(Height - 1, uint32_t((uint64_t(y) * Height) / height));
                for (uint32_t x = 0; x < width; ++x)
                {
                    const size_t offset = (size_t(y) * width + x) * 4;
                    const auto channel = [&](size_t c) {
                        const double value = rgba[offset + c];
                        return std::isfinite(value) ? std::max(value, 0.0) : 0.0;
                    };
                    const double luminance = .2126 * channel(0) + .7152 * channel(1) + .0722 * channel(2);
                    const uint32_t column = std::min(Width - 1, uint32_t((uint64_t(x) * Width) / width));
                    mass[row * Width + column] += luminance * solidAngle;
                }
            }
            double total = 0.0;
            for (double value : mass) total += value;
            if (!(total > 0.0) || !std::isfinite(total)) return {};
            auto result = std::make_shared<EnvironmentSamplingProposal>();
            double accumulated = 0.0;
            for (uint32_t index = 0; index < Cells; ++index)
            {
                accumulated += mass[index];
                result->Cdf[index + 1] = std::clamp(float(accumulated / total), result->Cdf[index], 1.0f);
            }
            result->Cdf.back() = 1.0f;
            return result;
        }
    };
}
