#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace HEngine
{
    // Shared frame budget, not a limit on an individual character's skeleton.
    inline constexpr uint32_t SceneBoneMatrixCapacity = 256u * 100u;

    inline uint32_t AppendSceneBonePalette(std::span<const glm::mat4> palette,
        std::span<const glm::mat4> previousPalette,
        std::vector<glm::mat4>& currentRows, std::vector<glm::mat4>& previousRows,
        uint32_t capacity = SceneBoneMatrixCapacity)
    {
        if (palette.empty() || currentRows.size() != previousRows.size())
            throw std::invalid_argument("SceneRenderer requires a nonempty, synchronized bone palette");
        if (currentRows.size() > capacity || palette.size() > capacity - currentRows.size())
            throw std::length_error("SceneRenderer skinned matrix capacity exceeded");

        const auto offset = static_cast<uint32_t>(currentRows.size());
        const auto count = currentRows.size() + palette.size();
        currentRows.reserve(count);
        previousRows.reserve(count);
        currentRows.insert(currentRows.end(), palette.begin(), palette.end());
        // Reloaded or newly visible skeletons must not read another character's
        // rows as history. Camera cuts supply an empty previous palette.
        const auto history = previousPalette.size() == palette.size() ? previousPalette : palette;
        previousRows.insert(previousRows.end(), history.begin(), history.end());
        return offset;
    }
}
