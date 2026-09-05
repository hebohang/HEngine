#pragma once

#include <cstdint>

namespace HEngine::EnvironmentMapSizes
{
    inline constexpr uint32_t Capture = 512;
    inline constexpr uint32_t Irradiance = 32;
    inline constexpr uint32_t Prefilter = 128;
    inline constexpr uint32_t BrdfLut = 256;
}
