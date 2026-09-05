#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string_view>
#include <vector>

namespace HEngine
{
    class ContentStore
    {
    public:
        virtual ~ContentStore();

        [[nodiscard]] virtual std::vector<std::uint8_t> ReadAll(
            std::string_view projectPath,
            std::size_t maxBytes,
            std::stop_token stopToken = {}) const = 0;

        // Invalid or unsafe project paths throw. A missing or non-file path returns nullopt.
        [[nodiscard]] virtual std::optional<std::uint64_t> TryGetSize(
            std::string_view projectPath) const = 0;
    };

    using ContentStoreSnapshot = std::shared_ptr<const ContentStore>;
}
