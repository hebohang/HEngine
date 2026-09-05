#pragma once

#include "Runtime/Resource/VirtualFileSystem/ContentStore.h"

#include <atomic>

namespace HEngine
{
    class VirtualFileSystem final
    {
    public:
        [[nodiscard]] static VirtualFileSystem& GetInstance();

        void Install(ContentStoreSnapshot snapshot) noexcept;
        void Clear() noexcept;
        [[nodiscard]] ContentStoreSnapshot Capture() const noexcept;

    private:
        VirtualFileSystem() = default;

        std::atomic<ContentStoreSnapshot> mSnapshot;
    };
}
