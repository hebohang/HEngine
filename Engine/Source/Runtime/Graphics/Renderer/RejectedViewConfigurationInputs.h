#pragma once

#include "Runtime/Graphics/Settings/RenderSettings.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
    // Only configuration input is retained: never a World pointer, frame number,
    // submission or animation state. One failed automatic transaction is bounded.
    struct RejectedViewConfigurationInputs
    {
        struct View
        {
            uint32_t Slot = 0;
            uint32_t Generation = 0;
            uint64_t SceneGeneration = 0;
            bool bHasWorld = false;
            std::string Kind;
            Json WorldOverrides;
            Json CameraOverrides;
        };
        std::string BaseHash;
        std::vector<View> Views;

        [[nodiscard]] bool MatchesBase(std::string_view hash) const { return BaseHash == hash; }
        [[nodiscard]] bool MatchesViewCount(size_t count) const { return Views.size() == count; }

        [[nodiscard]] bool MatchesView(size_t offset, uint32_t slot, uint32_t generation,
            uint64_t sceneGeneration, bool bHasWorld, std::string_view kind,
            const Json& world, const Json& camera) const
        {
            if (offset >= Views.size()) return false;
            const auto& input = Views[offset];
            return input.Slot == slot && input.Generation == generation &&
                input.SceneGeneration == sceneGeneration && input.bHasWorld == bHasWorld &&
                input.Kind == kind && input.WorldOverrides == world && input.CameraOverrides == camera;
        }
    };
}
