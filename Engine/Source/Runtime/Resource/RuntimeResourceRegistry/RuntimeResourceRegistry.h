#pragma once

#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeResourceTypes.h"

#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <stop_token>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <typeindex>
#include <utility>

namespace HEngine
{
    namespace Detail
    {
        struct RuntimeResourceEntry;

        void AddRuntimeResourceLease(const std::shared_ptr<RuntimeResourceEntry>& entry) noexcept;
        void ReleaseRuntimeResourceLease(const std::shared_ptr<RuntimeResourceEntry>& entry) noexcept;
        [[nodiscard]] std::shared_ptr<void> CreateRuntimeResourceSharedLease(
            const std::shared_ptr<RuntimeResourceEntry>& entry,
            void* resource);
        [[nodiscard]] const RuntimeResourceKey& GetRuntimeResourceEntryKey(
            const std::shared_ptr<RuntimeResourceEntry>& entry) noexcept;
        [[nodiscard]] uint64_t GetRuntimeResourceEntryGeneration(
            const std::shared_ptr<RuntimeResourceEntry>& entry) noexcept;

        struct RuntimeResourceAcquireResult
        {
            std::shared_ptr<RuntimeResourceEntry> Entry;
            void* Resource = nullptr;
        };
    }

    template<typename T>
    class RuntimeResourceHandle
    {
    public:
        RuntimeResourceHandle() noexcept = default;

        RuntimeResourceHandle(const RuntimeResourceHandle& other) noexcept
            : mEntry(other.mEntry), mResource(other.mResource)
        {
            Detail::AddRuntimeResourceLease(mEntry);
        }

        RuntimeResourceHandle(RuntimeResourceHandle&& other) noexcept
            : mEntry(std::move(other.mEntry)), mResource(std::exchange(other.mResource, nullptr))
        {
        }

        ~RuntimeResourceHandle()
        {
            Reset();
        }

        RuntimeResourceHandle& operator=(const RuntimeResourceHandle& other) noexcept
        {
            if (this == &other)
                return *this;

            RuntimeResourceHandle copy(other);
            Swap(copy);
            return *this;
        }

        RuntimeResourceHandle& operator=(RuntimeResourceHandle&& other) noexcept
        {
            if (this == &other)
                return *this;

            Reset();
            mEntry = std::move(other.mEntry);
            mResource = std::exchange(other.mResource, nullptr);
            return *this;
        }

        void Reset() noexcept
        {
            if (!mEntry)
                return;

            Detail::ReleaseRuntimeResourceLease(mEntry);
            mEntry.reset();
            mResource = nullptr;
        }

        void Swap(RuntimeResourceHandle& other) noexcept
        {
            mEntry.swap(other.mEntry);
            std::swap(mResource, other.mResource);
        }

        [[nodiscard]] T* Get() const noexcept
        {
            return mResource;
        }

        [[nodiscard]] std::shared_ptr<T> GetShared() const
        {
            if (!mEntry || mResource == nullptr)
                return {};
            return std::static_pointer_cast<T>(
                Detail::CreateRuntimeResourceSharedLease(mEntry, mResource));
        }

        [[nodiscard]] std::shared_ptr<T> GetRef() const
        {
            return GetShared();
        }

        [[nodiscard]] T* operator->() const noexcept
        {
            return Get();
        }

        [[nodiscard]] T& operator*() const
        {
            return *mResource;
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return mResource != nullptr;
        }

        [[nodiscard]] const RuntimeResourceKey& GetKey() const
        {
            if (!mEntry)
                throw std::logic_error("Runtime resource handle is empty.");
            return Detail::GetRuntimeResourceEntryKey(mEntry);
        }

        [[nodiscard]] uint64_t GetGeneration() const noexcept
        {
            return mEntry ? Detail::GetRuntimeResourceEntryGeneration(mEntry) : 0;
        }

    private:
        friend class RuntimeResourceRegistry;
        friend class RuntimeResourceGroup;

        RuntimeResourceHandle(std::shared_ptr<Detail::RuntimeResourceEntry> entry, T* resource) noexcept
            : mEntry(std::move(entry)), mResource(resource)
        {
            Detail::AddRuntimeResourceLease(mEntry);
        }

        std::shared_ptr<Detail::RuntimeResourceEntry> mEntry;
        T* mResource = nullptr;
    };

    class RuntimeResourceRegistry;

    class RuntimeResourceGroup
    {
    public:
        // Pins are generation-specific snapshots. Retry/Invalidate never move
        // an existing group pin to the replacement generation.
        RuntimeResourceGroup() noexcept = default;
        RuntimeResourceGroup(const RuntimeResourceGroup&) = delete;
        RuntimeResourceGroup& operator=(const RuntimeResourceGroup&) = delete;
        RuntimeResourceGroup(RuntimeResourceGroup&& other) noexcept;
        RuntimeResourceGroup& operator=(RuntimeResourceGroup&& other) noexcept;
        ~RuntimeResourceGroup();

        template<typename T>
        [[nodiscard]] bool Pin(const RuntimeResourceHandle<T>& handle);
        [[nodiscard]] bool Pin(const RuntimeResourceKey& key);
        [[nodiscard]] RuntimeResourceGroupProgress Progress() const;
        void Release() noexcept;
        [[nodiscard]] explicit operator bool() const noexcept;

    private:
        friend class RuntimeResourceRegistry;

        RuntimeResourceGroup(RuntimeResourceRegistry* registry, uint64_t id) noexcept;

        RuntimeResourceRegistry* mRegistry = nullptr;
        uint64_t mId = 0;
    };

    class RuntimeResourceRegistry final
    {
    public:
        // The registry must outlive all handles and groups. AcquireBlocking
        // loaders run on the caller; QueueAsync CPU loaders run on workers and
        // their finalizers run only through the Init-thread pump. Shutdown stops
        // new acquisitions and throws while leases are outstanding; release
        // handles/GetShared aliases, then call it again.
        RuntimeResourceRegistry();
        ~RuntimeResourceRegistry();
        RuntimeResourceRegistry(const RuntimeResourceRegistry&) = delete;
        RuntimeResourceRegistry& operator=(const RuntimeResourceRegistry&) = delete;

        [[nodiscard]] static RuntimeResourceRegistry& GetInstance();

        // Zero workers is a synchronous-only registry. QueueAsync rejects the
        // request before creating entries or pins; host IO may complete first.
        void Init(
            size_t workerCount = 2,
            std::thread::id initThreadId = std::this_thread::get_id());
        void Init(std::thread::id initThreadId)
        {
            Init(2, initThreadId);
        }
        [[nodiscard]] bool IsInitialized() const noexcept;

        template<typename T, typename Loader>
        [[nodiscard]] RuntimeResourceHandle<T> AcquireBlocking(
            const RuntimeResourceKey& key,
            Loader&& loader,
            size_t residentBytes = 0)
        {
            static_assert(!std::is_const_v<T>, "RuntimeResourceHandle<T> requires a non-const resource type.");

            using StoredLoader = std::decay_t<Loader>;
            using LoaderResult = std::invoke_result_t<StoredLoader&>;
            static_assert(
                std::is_convertible_v<LoaderResult, std::shared_ptr<T>>,
                "Runtime resource loader must return std::shared_ptr<T>.");

            auto loaderState = std::make_shared<StoredLoader>(std::forward<Loader>(loader));
            Detail::RuntimeResourceAcquireResult result = AcquireBlockingErased(
                key,
                std::type_index(typeid(T)),
                [loaderState]() -> std::shared_ptr<void>
                {
                    std::shared_ptr<T> resource = std::invoke(*loaderState);
                    return std::static_pointer_cast<void>(std::move(resource));
                },
                residentBytes);
            return RuntimeResourceHandle<T>(std::move(result.Entry), static_cast<T*>(result.Resource));
        }

        template<typename T, typename Payload, typename CpuLoader, typename MainThreadFinalizer>
        // Returns true only when this call queued the physical load. Returns
        // false for Ready cache hits, in-flight joins, and cached Failed or
        // EvictPending entries; call Retry/Invalidate before requesting a new generation.
        // The optional CPU stop token is scoped to that generation and is
        // requested by Invalidate and Shutdown.
        [[nodiscard]] bool QueueAsync(
            const RuntimeResourceKey& key,
            CpuLoader&& cpuLoader,
            MainThreadFinalizer&& mainThreadFinalizer,
            size_t residentBytes = 0)
        {
            return QueueAsyncImpl<T, Payload>(
                key,
                0,
                std::forward<CpuLoader>(cpuLoader),
                std::forward<MainThreadFinalizer>(mainThreadFinalizer),
                residentBytes);
        }

        // Resolves the current generation, pins it into group, and observes or
        // queues its load while holding one registry lock. This closes the
        // Ready cache-hit eviction window between QueueAsync and Group::Pin.
        template<typename T, typename Payload, typename CpuLoader, typename MainThreadFinalizer>
        [[nodiscard]] bool QueueAsyncPinned(
            const RuntimeResourceKey& key,
            RuntimeResourceGroup& group,
            CpuLoader&& cpuLoader,
            MainThreadFinalizer&& mainThreadFinalizer,
            size_t residentBytes = 0)
        {
            if (group.mRegistry != this || group.mId == 0)
            {
                throw std::logic_error(
                    "Pinned async runtime resource queue requires a group owned by this registry.");
            }

            return QueueAsyncImpl<T, Payload>(
                key,
                group.mId,
                std::forward<CpuLoader>(cpuLoader),
                std::forward<MainThreadFinalizer>(mainThreadFinalizer),
                residentBytes);
        }

    private:
        template<typename T, typename Payload, typename CpuLoader, typename MainThreadFinalizer>
        [[nodiscard]] bool QueueAsyncImpl(
            const RuntimeResourceKey& key,
            uint64_t pinGroupId,
            CpuLoader&& cpuLoader,
            MainThreadFinalizer&& mainThreadFinalizer,
            size_t residentBytes)
        {
            static_assert(!std::is_const_v<T>, "RuntimeResourceHandle<T> requires a non-const resource type.");
            static_assert(std::is_object_v<Payload> && !std::is_const_v<Payload>,
                "Async runtime resource payload must be a non-const object type.");

            using StoredCpuLoader = std::decay_t<CpuLoader>;
            using StoredFinalizer = std::decay_t<MainThreadFinalizer>;
            static_assert(
                std::is_invocable_v<StoredCpuLoader&, std::stop_token> ||
                    std::is_invocable_v<StoredCpuLoader&>,
                "CPU loader must be invocable with std::stop_token or with no arguments.");
            static_assert(
                std::is_invocable_v<StoredFinalizer&, Payload&&> ||
                    std::is_invocable_v<StoredFinalizer&, const Payload&>,
                "Main-thread finalizer must accept Payload&& or const Payload&.");

            auto cpuLoaderState = std::make_shared<StoredCpuLoader>(std::forward<CpuLoader>(cpuLoader));
            auto finalizerState = std::make_shared<StoredFinalizer>(
                std::forward<MainThreadFinalizer>(mainThreadFinalizer));

            std::function<std::shared_ptr<void>(std::stop_token)> erasedCpuLoader =
                [cpuLoaderState](std::stop_token stopToken) -> std::shared_ptr<void>
                {
                    if constexpr (std::is_invocable_v<StoredCpuLoader&, std::stop_token>)
                    {
                        using Result = std::invoke_result_t<StoredCpuLoader&, std::stop_token>;
                        static_assert(std::is_convertible_v<Result, Payload>,
                            "CPU loader result must be convertible to Payload.");
                        return std::make_shared<Payload>(std::invoke(*cpuLoaderState, stopToken));
                    }
                    else
                    {
                        using Result = std::invoke_result_t<StoredCpuLoader&>;
                        static_assert(std::is_convertible_v<Result, Payload>,
                            "CPU loader result must be convertible to Payload.");
                        return std::make_shared<Payload>(std::invoke(*cpuLoaderState));
                    }
                };

            std::function<std::shared_ptr<void>(std::shared_ptr<void>)> erasedFinalizer =
                [finalizerState](std::shared_ptr<void> erasedPayload) -> std::shared_ptr<void>
                {
                    std::shared_ptr<Payload> payload =
                        std::static_pointer_cast<Payload>(std::move(erasedPayload));
                    if constexpr (std::is_invocable_v<StoredFinalizer&, Payload&&>)
                    {
                        using Result = std::invoke_result_t<StoredFinalizer&, Payload&&>;
                        static_assert(std::is_convertible_v<Result, std::shared_ptr<T>>,
                            "Main-thread finalizer must return std::shared_ptr<T>.");
                        std::shared_ptr<T> resource =
                            std::invoke(*finalizerState, std::move(*payload));
                        return std::static_pointer_cast<void>(std::move(resource));
                    }
                    else
                    {
                        using Result = std::invoke_result_t<StoredFinalizer&, const Payload&>;
                        static_assert(std::is_convertible_v<Result, std::shared_ptr<T>>,
                            "Main-thread finalizer must return std::shared_ptr<T>.");
                        std::shared_ptr<T> resource = std::invoke(*finalizerState, *payload);
                        return std::static_pointer_cast<void>(std::move(resource));
                    }
                };

            return QueueAsyncErased(
                key,
                std::type_index(typeid(T)),
                std::move(erasedCpuLoader),
                std::move(erasedFinalizer),
                residentBytes,
                pinGroupId);
        }

    public:

        template<typename T>
        [[nodiscard]] RuntimeResourceHandle<T> TryAcquireReady(const RuntimeResourceKey& key)
        {
            static_assert(!std::is_const_v<T>, "RuntimeResourceHandle<T> requires a non-const resource type.");
            Detail::RuntimeResourceAcquireResult result =
                TryAcquireReadyErased(key, std::type_index(typeid(T)));
            return RuntimeResourceHandle<T>(std::move(result.Entry), static_cast<T*>(result.Resource));
        }

        // Finalizers and deferred payload/callback destruction run only here on
        // the Init thread. A non-positive budget means no time limit.
        [[nodiscard]] size_t PumpMainThreadWork(
            std::chrono::nanoseconds timeBudget = std::chrono::milliseconds(2),
            size_t maxItems = std::numeric_limits<size_t>::max());

        [[nodiscard]] RuntimeResourceState GetState(const RuntimeResourceKey& key) const;
        // Rethrows the cached exception for the current Failed generation.
        // Returns without throwing when the key has no current failed entry.
        void RethrowFailure(const RuntimeResourceKey& key) const;
        [[nodiscard]] bool Retry(const RuntimeResourceKey& key);
        // Init-thread only; detaches the exact generation, requests its cooperative
        // CPU stop token, and discards pending work.
        [[nodiscard]] bool Invalidate(const RuntimeResourceKey& key);

        [[nodiscard]] RuntimeResourceGroup CreateGroup();
        // Handle release only drops a lease. Resource reset occurs here or in
        // Shutdown, both on the Init thread.
        [[nodiscard]] size_t CollectUnused();
        // Must run on the Init thread. External RuntimeResourceGroup objects
        // become empty snapshots after this call and must not outlive registry.
        void Shutdown();

        [[nodiscard]] RuntimeResourceStats GetStats() const noexcept;

    private:
        friend class RuntimeResourceGroup;

        struct Impl;

        [[nodiscard]] Detail::RuntimeResourceAcquireResult AcquireBlockingErased(
            const RuntimeResourceKey& key,
            std::type_index resourceType,
            std::function<std::shared_ptr<void>()> loader,
            size_t residentBytes);
        [[nodiscard]] bool QueueAsyncErased(
            const RuntimeResourceKey& key,
            std::type_index resourceType,
            std::function<std::shared_ptr<void>(std::stop_token)> cpuLoader,
            std::function<std::shared_ptr<void>(std::shared_ptr<void>)> mainThreadFinalizer,
            size_t residentBytes,
            uint64_t pinGroupId);
        [[nodiscard]] Detail::RuntimeResourceAcquireResult TryAcquireReadyErased(
            const RuntimeResourceKey& key,
            std::type_index resourceType);
        [[nodiscard]] size_t PumpMainThreadWorkInternal(
            std::chrono::nanoseconds timeBudget,
            size_t maxItems,
            const std::shared_ptr<Detail::RuntimeResourceEntry>& targetEntry);
        void WorkerLoop(std::stop_token stopToken);
        [[nodiscard]] bool PinGroupEntry(
            uint64_t groupId,
            const std::shared_ptr<Detail::RuntimeResourceEntry>& entry);
        [[nodiscard]] bool PinGroupKey(uint64_t groupId, const RuntimeResourceKey& key);
        [[nodiscard]] RuntimeResourceGroupProgress GetGroupProgress(uint64_t groupId) const;
        void ReleaseGroup(uint64_t groupId) noexcept;

        std::unique_ptr<Impl> mImpl;
    };

    template<typename T>
    bool RuntimeResourceGroup::Pin(const RuntimeResourceHandle<T>& handle)
    {
        return mRegistry != nullptr && mId != 0 && handle.mEntry != nullptr &&
            mRegistry->PinGroupEntry(mId, handle.mEntry);
    }
}
