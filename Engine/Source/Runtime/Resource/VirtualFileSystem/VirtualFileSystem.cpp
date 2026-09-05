#include "Hepch.h"

#include "Runtime/Resource/VirtualFileSystem/VirtualFileSystem.h"

#include <utility>

namespace HEngine
{
    VirtualFileSystem& VirtualFileSystem::GetInstance()
    {
        static VirtualFileSystem instance;
        return instance;
    }

    void VirtualFileSystem::Install(ContentStoreSnapshot snapshot) noexcept
    {
        mSnapshot.store(std::move(snapshot), std::memory_order_release);
    }

    void VirtualFileSystem::Clear() noexcept
    {
        Install({});
    }

    ContentStoreSnapshot VirtualFileSystem::Capture() const noexcept
    {
        return mSnapshot.load(std::memory_order_acquire);
    }
}
