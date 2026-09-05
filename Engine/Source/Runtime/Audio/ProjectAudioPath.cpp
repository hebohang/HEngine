#include "Hepch.h"

#include "Runtime/Audio/ProjectAudioPath.h"

namespace HEngine
{
    Utils::PathValidationResult TryResolveProjectAudioPath(
        const std::filesystem::path& projectRoot,
        std::string_view input,
        ProjectAudioPath& outPath)
    {
        outPath = {};
        return Utils::TryResolveProjectRelativePath(
            projectRoot,
            input,
            outPath.FullPath,
            outPath.NormalizedPath);
    }
}
