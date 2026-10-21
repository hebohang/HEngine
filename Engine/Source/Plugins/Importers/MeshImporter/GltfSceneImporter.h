#pragma once
#include "Runtime/Core/Plugins/Services/SceneImporterService.h"

namespace HEngine
{
    class GltfSceneImporter final : public ISceneImporterService
    {
    public:
        Json Convert(const std::filesystem::path& source,
            const std::filesystem::path& staging) override;
    };
}
