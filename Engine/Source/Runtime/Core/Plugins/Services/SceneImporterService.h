#pragma once
#include "Runtime/Core/Meta/Json.h"
#include <filesystem>

namespace HEngine
{
    inline constexpr const char* c_SceneImporterServiceId = "he.service.scene-importer";
    // Tool-only conversion. The returned document contains native reflected values,
    // source identities, slot identities and diagnostics; publication is a separate step.
    class ISceneImporterService
    {
    public:
        virtual ~ISceneImporterService() = default;
        virtual Json Convert(const std::filesystem::path& source,
            const std::filesystem::path& staging) = 0;
    };
}
