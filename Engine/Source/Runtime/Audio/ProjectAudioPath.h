#pragma once

#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace HEngine
{
    struct ProjectAudioPath
    {
        std::filesystem::path FullPath;
        std::string NormalizedPath;
    };

    [[nodiscard]] Utils::PathValidationResult TryResolveProjectAudioPath(
        const std::filesystem::path& projectRoot,
        std::string_view input,
        ProjectAudioPath& outPath);
}
