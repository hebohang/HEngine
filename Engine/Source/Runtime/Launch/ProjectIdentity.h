#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <string_view>

namespace HEngine
{
    struct ProjectIdentity
    {
        std::string ProjectId;
        std::string DisplayName;
        std::string ExecutableName;
        std::string GameConfig;
        std::string LegacySavedDirectory;
    };

    // Shared by portable loose projects and cooked player metadata. No path is
    // used for save storage or configuration loading before this validation.
    [[nodiscard]] ProjectIdentity ReadProjectIdentity(
        const nlohmann::json& descriptor, std::string_view engineVersion);
    [[nodiscard]] ProjectIdentity ReadPlayerIdentity(
        const nlohmann::json& metadata, std::string_view engineVersion,
        std::string_view runtimeFingerprint);
}
