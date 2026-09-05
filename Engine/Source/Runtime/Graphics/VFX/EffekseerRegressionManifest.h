#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
    namespace EffekseerRegressionManifestLimits
    {
        inline constexpr std::uint32_t SchemaVersion = 1;
        inline constexpr std::size_t MaxPackages = 64;
        inline constexpr std::string_view DefaultProjectPath =
            "Config/EffekseerRegression.json";
    }

    struct EffekseerRegressionManifest
    {
        std::vector<std::string> Packages;
    };

    [[nodiscard]] EffekseerRegressionManifest LoadEffekseerRegressionManifest(
        const std::filesystem::path& projectRoot,
        std::string_view manifestProjectPath =
            EffekseerRegressionManifestLimits::DefaultProjectPath);
}
