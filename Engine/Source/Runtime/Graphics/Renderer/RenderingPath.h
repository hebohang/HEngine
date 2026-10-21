#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace HEngine
{
    // Independent of the RHI backend and per-object submission classification.
    enum class RenderingPath : std::uint8_t
    {
        Forward,
        Deferred,
    };

    [[nodiscard]] inline const char* ToString(RenderingPath path) noexcept
    {
        return path == RenderingPath::Forward ? "Forward" : "Deferred";
    }

    [[nodiscard]] inline RenderingPath ParseRenderingPath(std::string_view value)
    {
        if (value == "Forward") return RenderingPath::Forward;
        if (value == "Deferred") return RenderingPath::Deferred;
        throw std::invalid_argument("Rendering.Path must be Forward or Deferred");
    }
}
