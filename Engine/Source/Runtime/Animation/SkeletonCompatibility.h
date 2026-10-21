#pragma once

#include "Runtime/Animation/Animdata.h"

#include <cmath>
#include <map>
#include <set>
#include <string>

namespace HEngine
{
    // A wardrobe may share an animator only when both skin palettes use exactly
    // the same slots and inverse bind poses. Matching bone names alone is unsafe.
    [[nodiscard]] inline std::string GetSkinPaletteIncompatibility(
        const std::map<std::string, BoneInfo>& source,
        const std::map<std::string, BoneInfo>& destination)
    {
        if (source.empty() || source.size() != destination.size())
            return "Skin palette bone count differs or is empty.";
        std::set<int> slots;
        for (const auto& [name, bone] : source)
        {
            const auto found = destination.find(name);
            if (found == destination.end() || found->second.id != bone.id)
                return "Skin palette mapping differs: " + name;
            if (bone.id < 0 || !slots.insert(bone.id).second)
                return "Skin palette slot is invalid or duplicated: " + name;
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                {
                    const float sourceValue = bone.offset[column][row];
                    const float destinationValue = found->second.offset[column][row];
                    if (!std::isfinite(sourceValue) || !std::isfinite(destinationValue) ||
                        std::abs(sourceValue - destinationValue) > 0.0001f)
                        return "Skin palette inverse bind pose differs or is invalid: " + name;
                }
        }
        return {};
    }
}
