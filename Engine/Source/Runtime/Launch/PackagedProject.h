#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

namespace HEngine
{
    class RuntimeResourceCatalogSnapshot;

    struct PackagedProjectPaths
    {
        std::filesystem::path contentRoot;
        std::filesystem::path savedRoot;
        std::shared_ptr<const RuntimeResourceCatalogSnapshot> resourceCatalog;
        std::filesystem::path gameConfig;
    };

    class PackagedProject final
    {
    public:
        [[nodiscard]] static PackagedProjectPaths Prepare(
            std::string_view packageCheckId = {});
    };
}
