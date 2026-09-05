#pragma once

#include <filesystem>

namespace HEngine
{
    inline constexpr const char* c_MeshImporterServiceId =
        "he.service.mesh-importer";

    class IMeshImporterService
    {
    public:
        virtual ~IMeshImporterService() = default;

        [[nodiscard]] virtual bool Import(
            const std::filesystem::path& assetPath,
            const std::filesystem::path& outputDirectory) = 0;
    };
}
