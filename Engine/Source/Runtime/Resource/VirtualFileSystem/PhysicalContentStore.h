#pragma once

#include "Runtime/Resource/VirtualFileSystem/ContentStore.h"

#include <filesystem>

namespace HEngine
{
    class PhysicalContentStore final : public ContentStore
    {
    public:
        explicit PhysicalContentStore(std::filesystem::path projectRoot);

        [[nodiscard]] std::vector<std::uint8_t> ReadAll(
            std::string_view projectPath,
            std::size_t maxBytes,
            std::stop_token stopToken = {}) const override;
        [[nodiscard]] std::optional<std::uint64_t> TryGetSize(
            std::string_view projectPath) const override;

    private:
        [[nodiscard]] std::filesystem::path Resolve(std::string_view projectPath) const;

        const std::filesystem::path mProjectRoot;
    };
}
