#pragma once

#include "Hepch.h"
#include "Runtime/Core/Base/Base.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <type_traits>

namespace HEngine
{
#define DECLARE_MULTICAST_DELEGATE(name, ...) using name = MultiCastDelegate<__VA_ARGS__>;

    class DelegateHandle
    {
    public:
        DelegateHandle() = default;
        explicit DelegateHandle(int) : mId(GenerateID()) {}
        DelegateHandle(const DelegateHandle&) = default;
        DelegateHandle(DelegateHandle&& other) noexcept : mId(other.mId) { other.Reset(); }
        DelegateHandle& operator=(const DelegateHandle&) = default;
        DelegateHandle& operator=(DelegateHandle&& other) noexcept
        {
            if (this != &other) { mId = other.mId; other.Reset(); }
            return *this;
        }
        explicit operator bool() const noexcept { return IsValid(); }
        bool operator==(const DelegateHandle&) const = default;
        bool operator<(const DelegateHandle& other) const noexcept { return mId < other.mId; }
        bool IsValid() const noexcept { return mId != InvalidId; }
        void Reset() noexcept { mId = InvalidId; }

    private:
        static constexpr size_t InvalidId = size_t(-1);
        static size_t GenerateID() noexcept
        {
            static std::atomic_size_t nextId{0};
            return nextId.fetch_add(1, std::memory_order_relaxed);
        }
        size_t mId = InvalidId;
    };

    // Management is safe across threads. Callbacks run without the management lock;
    // callers must synchronize callback-owned state if broadcasts can overlap.
    // A broadcast snapshots its subscribers: additions appear on the next (including
    // nested) broadcast, removals skip callbacks whose active check has not run,
    // and exceptions
    // propagate without leaving a broadcast state to repair.
    template<typename... Args>
    class MultiCastDelegate
    {
        static_assert((!std::is_rvalue_reference_v<Args> && ...),
            "Multicast delegates cannot replay rvalue-reference arguments");
        static_assert(((std::is_reference_v<Args> || std::is_copy_constructible_v<Args>) && ...),
            "Multicast by-value arguments must be copy constructible");
        using DelegateType = std::function<void(Args...)>;
        struct Entry
        {
            DelegateHandle Handle;
            DelegateType Callback;
            std::atomic_bool Active{true};
            Entry(DelegateHandle handle, DelegateType callback)
                : Handle(handle), Callback(std::move(callback)) {}
        };

    public:
        MultiCastDelegate() = default;
        MultiCastDelegate(const MultiCastDelegate&) = delete;
        MultiCastDelegate& operator=(const MultiCastDelegate&) = delete;

        template<typename F> requires std::is_constructible_v<DelegateType, F>
        [[nodiscard]] DelegateHandle Add(F&& callable)
        {
            DelegateHandle handle(0);
            auto entry = std::make_shared<Entry>(handle, DelegateType(std::forward<F>(callable)));
            std::lock_guard lock(mMutex);
            mEntries.push_back(std::move(entry));
            return handle;
        }

        // The caller must remove the handle and quiesce in-flight broadcasts
        // before instance destruction.
        template<typename T>
        [[nodiscard]] DelegateHandle AddMember(void(T::* member)(Args...), T& instance)
        {
            return Add([&instance, member](Args... args)
            {
                (instance.*member)(std::forward<Args>(args)...);
            });
        }

        template<typename T>
        [[nodiscard]] DelegateHandle AddWeakMember(
            void(T::* member)(Args...), std::weak_ptr<T> instance)
        {
            return Add([instance = std::move(instance), member](Args... args)
            {
                if (auto owner = instance.lock())
                    ((*owner).*member)(std::forward<Args>(args)...);
            });
        }

        template<typename F> requires std::is_constructible_v<DelegateType, F>
        [[nodiscard]] DelegateHandle operator+=(F&& callable)
        {
            return Add(std::forward<F>(callable));
        }

        bool operator-=(DelegateHandle& handle) { return Remove(handle); }
        bool Remove(DelegateHandle& handle)
        {
            if (!handle.IsValid()) return false;
            std::lock_guard lock(mMutex);
            const auto found = std::find_if(mEntries.begin(), mEntries.end(),
                [&handle](const auto& entry) { return entry->Handle == handle; });
            if (found == mEntries.end()) return false;
            (*found)->Active.store(false, std::memory_order_release);
            mEntries.erase(found);
            handle.Reset();
            return true;
        }

        void RemoveAll()
        {
            std::lock_guard lock(mMutex);
            for (const auto& entry : mEntries)
                entry->Active.store(false, std::memory_order_release);
            mEntries.clear();
        }

        void Broadcast(Args... args)
        {
            std::vector<std::shared_ptr<Entry>> snapshot;
            {
                std::lock_guard lock(mMutex);
                snapshot = mEntries;
            }
            for (const auto& entry : snapshot)
                if (entry->Active.load(std::memory_order_acquire))
                    entry->Callback(args...);
        }

        bool IsHandleBound(const DelegateHandle& handle) const
        {
            if (!handle.IsValid()) return false;
            std::lock_guard lock(mMutex);
            return std::any_of(mEntries.begin(), mEntries.end(),
                [&handle](const auto& entry) { return entry->Handle == handle; });
        }

    private:
        mutable std::mutex mMutex;
        std::vector<std::shared_ptr<Entry>> mEntries;
    };
}
