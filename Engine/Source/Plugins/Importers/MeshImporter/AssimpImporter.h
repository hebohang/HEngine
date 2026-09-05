#pragma once

#include <filesystem>

namespace HEngine
{
    class AssimpImporter
    {
    public:
        bool Import(const std::filesystem::path& assetPath, const std::filesystem::path& outDir);
    };
}
