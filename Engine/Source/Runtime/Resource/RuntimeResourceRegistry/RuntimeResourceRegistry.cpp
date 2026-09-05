#include "Hepch.h"

#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeResourceRegistry.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] size_t CombineHash(size_t seed, size_t value) noexcept
        {
            return seed ^ (value + static_cast<size_t>(0x9e3779b9) + (seed << 6) + (seed >> 2));
        }

        void ValidateNoEmbeddedNull(std::string_view value, const char* label)
        {
            if (value.find('\0') != std::string_view::npos)
                throw std::invalid_argument(std::string(label) + " cannot contain an embedded null.");
        }

        void ValidateRuntimeResourceKey(const RuntimeResourceKey& key)
        {
            ValidateNoEmbeddedNull(key.Variant, "Runtime resource variant");

            if (key.bBuiltin)
            {
                if (key.Path.empty())
                    throw std::invalid_argument("Builtin runtime resource name cannot be empty.");
                ValidateNoEmbeddedNull(key.Path, "Builtin runtime resource name");
                if (key.CachePath != key.Path)
                    throw std::invalid_argument("Builtin runtime resource key is not canonical.");
                return;
            }

            std::string normalizedPath;
            const Utils::PathValidationResult result =
                Utils::TryNormalizeProjectRelativePath(key.Path, normalizedPath);
            if (!result)
                throw std::invalid_argument(result.Message);
            if (normalizedPath != key.Path || Utils::MakeProjectPathCacheKey(normalizedPath) != key.CachePath)
                throw std::invalid_argument("Project-relative runtime resource key is not canonical.");
        }

        [[nodiscard]] uint64_t AllocateRegistryId() noexcept
        {
            static std::atomic<uint64_t> nextId = 1;
            return nextId.fetch_add(1, std::memory_order_relaxed);
        }

        thread_local bool bInsideRuntimeResourceCpuLoader = false;
        thread_local bool bInsideRuntimeResourceFinalizer = false;

        class ScopedRuntimeResourceCallback final
        {
        public:
            explicit ScopedRuntimeResourceCallback(bool& callbackFlag) noexcept
                : mCallbackFlag(callbackFlag), mPreviousValue(callbackFlag)
            {
                mCallbackFlag = true;
            }

            ~ScopedRuntimeResourceCallback()
            {
                mCallbackFlag = mPreviousValue;
            }

        private:
            bool& mCallbackFlag;
            bool mPreviousValue = false;
        };
    }

    RuntimeResourceKey RuntimeResourceKey::FromProjectPath(
        RuntimeResourceKind kind,
        std::string_view path,
        std::string_view variant)
    {
        ValidateNoEmbeddedNull(variant, "Runtime resource variant");

        std::string normalizedPath;
        const Utils::PathValidationResult result =
            Utils::TryNormalizeProjectRelativePath(path, normalizedPath);
        if (!result)
            throw std::invalid_argument(result.Message);

        RuntimeResourceKey key;
        key.Kind = kind;
        key.Path = std::move(normalizedPath);
        key.CachePath = Utils::MakeProjectPathCacheKey(key.Path);
        key.Variant = variant;
        return key;
    }

    RuntimeResourceKey RuntimeResourceKey::Builtin(
        RuntimeResourceKind kind,
        std::string_view name,
        std::string_view variant)
    {
        if (name.empty())
            throw std::invalid_argument("Builtin runtime resource name cannot be empty.");
        ValidateNoEmbeddedNull(name, "Builtin runtime resource name");
        ValidateNoEmbeddedNull(variant, "Runtime resource variant");

        RuntimeResourceKey key;
        key.Kind = kind;
        key.Path = name;
        key.CachePath = key.Path;
        key.Variant = variant;
        key.bBuiltin = true;
        return key;
    }

    bool RuntimeResourceKey::operator==(const RuntimeResourceKey& other) const noexcept
    {
        return Kind == other.Kind && bBuiltin == other.bBuiltin &&
            CachePath == other.CachePath && Variant == other.Variant;
    }

    size_t RuntimeResourceKeyHash::operator()(const RuntimeResourceKey& key) const noexcept
    {
        size_t hash = std::hash<uint8_t>{}(static_cast<uint8_t>(key.Kind));
        hash = CombineHash(hash, std::hash<bool>{}(key.bBuiltin));
        hash = CombineHash(hash, std::hash<std::string>{}(key.CachePath));
        return CombineHash(hash, std::hash<std::string>{}(key.Variant));
    }

    namespace Detail
    {
        struct RuntimeResourceEntry
        {
            RuntimeResourceKey ResourceKey;
            uint64_t Generation = 0;
            std::atomic<RuntimeResourceState> State = RuntimeResourceState::Unloaded;
            std::type_index ResourceType = std::type_index(typeid(void));
            std::shared_ptr<void> Resource;
            std::exception_ptr Failure;
            size_t ResidentBytes = 0;
            std::atomic<size_t> LeaseCount = 0;
            std::atomic<size_t> PinCount = 0;
            std::thread::id LoaderThreadId;
            std::thread::id FinalizerThreadId;
            uint64_t OwnerId = 0;
            bool bRetired = false;
            bool bCancelRequested = false;
            bool bLoadActive = false;
            std::stop_source LoadStopSource;
            std::condition_variable Completion;
        };

        void AddRuntimeResourceLease(const std::shared_ptr<RuntimeResourceEntry>& entry) noexcept
        {
            if (entry)
                entry->LeaseCount.fetch_add(1, std::memory_order_relaxed);
        }

        void ReleaseRuntimeResourceLease(const std::shared_ptr<RuntimeResourceEntry>& entry) noexcept
        {
            if (entry)
                entry->LeaseCount.fetch_sub(1, std::memory_order_release);
        }

        const RuntimeResourceKey& GetRuntimeResourceEntryKey(
            const std::shared_ptr<RuntimeResourceEntry>& entry) noexcept
        {
            return entry->ResourceKey;
        }

        uint64_t GetRuntimeResourceEntryGeneration(
            const std::shared_ptr<RuntimeResourceEntry>& entry) noexcept
        {
            return entry->Generation;
        }

        std::shared_ptr<void> CreateRuntimeResourceSharedLease(
            const std::shared_ptr<RuntimeResourceEntry>& entry,
            void* resource)
        {
            struct SharedLease
            {
                explicit SharedLease(std::shared_ptr<RuntimeResourceEntry> leaseEntry)
                    : Entry(std::move(leaseEntry))
                {
                    AddRuntimeResourceLease(Entry);
                }

                ~SharedLease()
                {
                    ReleaseRuntimeResourceLease(Entry);
                }

                std::shared_ptr<RuntimeResourceEntry> Entry;
            };

            auto lease = std::make_shared<SharedLease>(entry);
            return std::shared_ptr<void>(std::move(lease), resource);
        }
    }

    struct RuntimeResourceRegistry::Impl
    {
        struct CpuJob
        {
            std::shared_ptr<Detail::RuntimeResourceEntry> Entry;
            std::function<std::shared_ptr<void>(std::stop_token)> Loader;
            std::function<std::shared_ptr<void>(std::shared_ptr<void>)> Finalizer;
            size_t ResidentBytes = 0;
        };

        struct MainThreadWork
        {
            std::shared_ptr<Detail::RuntimeResourceEntry> Entry;
            std::shared_ptr<void> Payload;
            std::function<std::shared_ptr<void>(std::shared_ptr<void>)> Finalizer;
            size_t ResidentBytes = 0;
        };

        struct Slot
        {
            uint64_t NextGeneration = 1;
            std::shared_ptr<Detail::RuntimeResourceEntry> Current;
        };

        struct Group
        {
            std::vector<std::shared_ptr<Detail::RuntimeResourceEntry>> Entries;
            std::unordered_set<Detail::RuntimeResourceEntry*> UniqueEntries;
        };

        mutable std::mutex Mutex;
        std::condition_variable LoadsCompleted;
        std::unordered_map<RuntimeResourceKey, Slot, RuntimeResourceKeyHash> Slots;
        std::vector<std::shared_ptr<Detail::RuntimeResourceEntry>> RetiredEntries;
        std::unordered_map<uint64_t, Group> Groups;
        std::deque<CpuJob> CpuJobs;
        std::deque<MainThreadWork> MainThreadWorkQueue;
        std::deque<CpuJob> DeferredMainThreadCpuJobs;
        std::vector<std::shared_ptr<void>> DeferredMainThreadReleases;
        std::vector<std::jthread> Workers;
        std::condition_variable WorkAvailable;
        std::thread::id InitThreadId;
        bool bInitialized = false;
        bool bAcceptingRequests = false;
        bool bStopWorkers = false;
        size_t ActiveLoads = 0;
        uint64_t NextGroupId = 1;
        uint64_t RegistryId = 0;
        RuntimeResourceStats Stats;
    };

    namespace
    {
        template<typename ImplType, typename SlotType>
        [[nodiscard]] std::shared_ptr<Detail::RuntimeResourceEntry> CreateEntry(
            ImplType& impl,
            SlotType& slot,
            const RuntimeResourceKey& key)
        {
            auto entry = std::make_shared<Detail::RuntimeResourceEntry>();
            entry->ResourceKey = key;
            entry->Generation = slot.NextGeneration++;
            entry->OwnerId = impl.RegistryId;
            slot.Current = entry;
            return entry;
        }

        template<typename ImplType>
        void VerifyInitThread(const ImplType& impl, const char* operation)
        {
            if (impl.InitThreadId == std::thread::id{} ||
                std::this_thread::get_id() != impl.InitThreadId)
            {
                throw std::logic_error(std::string(operation) + " must run on the registry Init thread.");
            }
        }

        [[nodiscard]] bool IsLoadTerminal(RuntimeResourceState state) noexcept
        {
            return state == RuntimeResourceState::Ready ||
                state == RuntimeResourceState::Failed ||
                state == RuntimeResourceState::EvictPending;
        }

        void ThrowEntryFailure(const Detail::RuntimeResourceEntry& entry)
        {
            if (entry.Failure)
                std::rethrow_exception(entry.Failure);
            throw std::runtime_error("Runtime resource load failed without an exception.");
        }

        template<typename ImplType>
        void MoveResourceToReleaseList(
            ImplType& impl,
            const std::shared_ptr<Detail::RuntimeResourceEntry>& entry,
            std::vector<std::shared_ptr<void>>& releaseList)
        {
            if (!entry->Resource)
                return;

            releaseList.emplace_back(std::move(entry->Resource));
            if (impl.Stats.Resident > 0)
                --impl.Stats.Resident;
            if (entry->ResidentBytes <= impl.Stats.ResidentBytes)
                impl.Stats.ResidentBytes -= entry->ResidentBytes;
            else
                impl.Stats.ResidentBytes = 0;
            ++impl.Stats.Evictions;
            entry->ResidentBytes = 0;
            entry->State.store(RuntimeResourceState::EvictPending, std::memory_order_release);
        }

        void AddResidentBytesChecked(RuntimeResourceStats& stats, size_t residentBytes) noexcept
        {
            const uint64_t bytes = static_cast<uint64_t>(residentBytes);
            if (bytes > std::numeric_limits<uint64_t>::max() - stats.ResidentBytes)
                stats.ResidentBytes = std::numeric_limits<uint64_t>::max();
            else
                stats.ResidentBytes += bytes;
        }

        template<typename ImplType>
        void FinishActiveLoad(ImplType& impl, const std::shared_ptr<Detail::RuntimeResourceEntry>& entry)
        {
            if (!entry->bLoadActive)
                return;
            entry->bLoadActive = false;
            if (impl.ActiveLoads > 0)
                --impl.ActiveLoads;
        }

        template<typename GroupType>
        [[nodiscard]] bool PinGroupEntryStrong(
            GroupType& group,
            const std::shared_ptr<Detail::RuntimeResourceEntry>& entry,
            bool* outInserted = nullptr)
        {
            if (outInserted)
                *outInserted = false;
            if (group.UniqueEntries.find(entry.get()) != group.UniqueEntries.end())
                return true;

            group.Entries.emplace_back(entry);
            try
            {
                const auto [entryIt, bInserted] = group.UniqueEntries.emplace(entry.get());
                (void)entryIt;
                if (!bInserted)
                {
                    group.Entries.pop_back();
                    return true;
                }
            }
            catch (...)
            {
                group.Entries.pop_back();
                throw;
            }

            entry->PinCount.fetch_add(1, std::memory_order_relaxed);
            if (outInserted)
                *outInserted = true;
            return true;
        }

        template<typename GroupType>
        void UnpinGroupEntryStrong(
            GroupType& group,
            const std::shared_ptr<Detail::RuntimeResourceEntry>& entry) noexcept
        {
            if (group.UniqueEntries.erase(entry.get()) == 0)
                return;

            const auto entryIt = std::find(group.Entries.begin(), group.Entries.end(), entry);
            if (entryIt != group.Entries.end())
                group.Entries.erase(entryIt);
            entry->PinCount.fetch_sub(1, std::memory_order_release);
        }
    }

    RuntimeResourceRegistry::RuntimeResourceRegistry()
        : mImpl(std::make_unique<Impl>())
    {
        mImpl->RegistryId = AllocateRegistryId();
    }

    RuntimeResourceRegistry& RuntimeResourceRegistry::GetInstance()
    {
        static RuntimeResourceRegistry registry;
        return registry;
    }

    RuntimeResourceRegistry::~RuntimeResourceRegistry()
    {
        if (!mImpl)
            return;

        bool bHasRegistryState = false;
        {
            std::lock_guard lock(mImpl->Mutex);
            bHasRegistryState = mImpl->bInitialized || !mImpl->Slots.empty() ||
                !mImpl->RetiredEntries.empty();
        }

        if (!bHasRegistryState)
            return;
        if (mImpl->InitThreadId == std::thread::id{} ||
            std::this_thread::get_id() != mImpl->InitThreadId)
        {
            std::terminate();
        }

        if (IsInitialized())
            Shutdown();
        CollectUnused();

        std::lock_guard lock(mImpl->Mutex);
        if (!mImpl->RetiredEntries.empty())
            std::terminate();
    }

    void RuntimeResourceRegistry::Init(size_t workerCount, std::thread::id initThreadId)
    {
        if (workerCount == 0)
            throw std::invalid_argument("Runtime resource registry worker count must be greater than zero.");
        if (initThreadId == std::thread::id{})
            throw std::invalid_argument("Runtime resource registry Init thread cannot be empty.");

        std::unique_lock lock(mImpl->Mutex);
        if (mImpl->bInitialized)
        {
            if (mImpl->InitThreadId != initThreadId)
                throw std::logic_error("Runtime resource registry is already initialized on another thread.");
            return;
        }
        if (std::this_thread::get_id() != initThreadId)
            throw std::logic_error("Runtime resource registry must be initialized by its release thread.");
        if (mImpl->InitThreadId != std::thread::id{} && mImpl->InitThreadId != initThreadId)
            throw std::logic_error("Runtime resource registry cannot change its release thread.");

        mImpl->InitThreadId = initThreadId;
        mImpl->bInitialized = true;
        mImpl->bAcceptingRequests = true;
        mImpl->bStopWorkers = false;

        try
        {
            mImpl->Workers.reserve(workerCount);
            for (size_t index = 0; index < workerCount; ++index)
            {
                (void)index;
                mImpl->Workers.emplace_back([this](std::stop_token stopToken)
                {
                    WorkerLoop(stopToken);
                });
            }
        }
        catch (...)
        {
            mImpl->bAcceptingRequests = false;
            mImpl->bInitialized = false;
            mImpl->bStopWorkers = true;
            for (std::jthread& worker : mImpl->Workers)
                worker.request_stop();
            std::vector<std::jthread> workers = std::move(mImpl->Workers);
            lock.unlock();
            mImpl->WorkAvailable.notify_all();
            workers.clear();
            throw;
        }
    }

    bool RuntimeResourceRegistry::IsInitialized() const noexcept
    {
        std::lock_guard lock(mImpl->Mutex);
        return mImpl->bInitialized;
    }

    Detail::RuntimeResourceAcquireResult RuntimeResourceRegistry::AcquireBlockingErased(
        const RuntimeResourceKey& key,
        std::type_index resourceType,
        std::function<std::shared_ptr<void>()> loader,
        size_t residentBytes)
    {
        ValidateRuntimeResourceKey(key);
        if (!loader)
            throw std::invalid_argument("Runtime resource loader cannot be empty.");

        std::shared_ptr<Detail::RuntimeResourceEntry> entry;
        bool bRunLoader = false;
        bool bJoinedInflight = false;

        {
            std::unique_lock lock(mImpl->Mutex);
            if (!mImpl->bInitialized || !mImpl->bAcceptingRequests)
                throw std::logic_error("Runtime resource registry is not accepting requests.");

            Impl::Slot& slot = mImpl->Slots[key];
            entry = slot.Current;
            if (!entry)
                entry = CreateEntry(*mImpl, slot, key);

            RuntimeResourceState state = entry->State.load(std::memory_order_acquire);
            if (entry->ResourceType != std::type_index(typeid(void)) && entry->ResourceType != resourceType)
                throw std::invalid_argument("Runtime resource key was requested with a different resource type.");
            if ((bInsideRuntimeResourceCpuLoader || bInsideRuntimeResourceFinalizer) &&
                state != RuntimeResourceState::Ready && state != RuntimeResourceState::Failed)
            {
                throw std::logic_error(
                    "Blocking runtime resource dependencies are not allowed inside loaders or finalizers.");
            }

            if (state == RuntimeResourceState::Unloaded)
            {
                entry->ResourceType = resourceType;
                entry->State.store(RuntimeResourceState::LoadingCpu, std::memory_order_release);
                entry->LoaderThreadId = std::this_thread::get_id();
                entry->bLoadActive = true;
                ++mImpl->ActiveLoads;
                ++mImpl->Stats.PhysicalLoads;
                bRunLoader = true;
            }
            else if (state == RuntimeResourceState::LoadingCpu ||
                state == RuntimeResourceState::Queued ||
                state == RuntimeResourceState::PendingMainThread)
            {
                if (entry->LoaderThreadId == std::this_thread::get_id())
                    throw std::logic_error("Recursive blocking load detected for the same runtime resource key.");
                ++mImpl->Stats.InflightJoins;
                bJoinedInflight = true;
                while (!IsLoadTerminal(state))
                {
                    if (std::this_thread::get_id() == mImpl->InitThreadId)
                    {
                        if (state == RuntimeResourceState::PendingMainThread)
                        {
                            lock.unlock();
                            (void)PumpMainThreadWorkInternal(
                                std::chrono::nanoseconds::zero(), 1, entry);
                            lock.lock();
                        }
                        else
                        {
                            entry->Completion.wait(lock, [&entry]
                            {
                                const RuntimeResourceState current =
                                    entry->State.load(std::memory_order_acquire);
                                return current == RuntimeResourceState::PendingMainThread ||
                                    IsLoadTerminal(current);
                            });
                        }
                    }
                    else
                    {
                        entry->Completion.wait(lock, [&entry]
                        {
                            return IsLoadTerminal(entry->State.load(std::memory_order_acquire));
                        });
                    }
                    state = entry->State.load(std::memory_order_acquire);
                }
            }

            if (!bRunLoader)
            {
                if (state == RuntimeResourceState::Failed)
                    ThrowEntryFailure(*entry);
                if (state == RuntimeResourceState::EvictPending)
                {
                    throw std::runtime_error(
                        "Runtime resource load was cancelled before publication: " + key.Path);
                }
                if (state != RuntimeResourceState::Ready || !entry->Resource)
                    throw std::logic_error("Runtime resource entry did not reach a usable state.");

                if (!bJoinedInflight)
                    ++mImpl->Stats.CacheHits;
                return { entry, entry->Resource.get() };
            }
        }

        std::shared_ptr<void> loadedResource;
        std::exception_ptr failure;
        try
        {
            const ScopedRuntimeResourceCallback callbackScope(bInsideRuntimeResourceCpuLoader);
            loadedResource = loader();
            if (!loadedResource)
                throw std::runtime_error("Runtime resource loader returned null.");
        }
        catch (...)
        {
            failure = std::current_exception();
        }

        bool bDiscarded = false;
        {
            std::lock_guard lock(mImpl->Mutex);
            entry->LoaderThreadId = {};
            bDiscarded = entry->bRetired || entry->bCancelRequested ||
                !mImpl->bAcceptingRequests;
            if (bDiscarded)
            {
                if (loadedResource)
                    mImpl->DeferredMainThreadReleases.emplace_back(std::move(loadedResource));
                entry->State.store(RuntimeResourceState::EvictPending, std::memory_order_release);
            }
            else if (failure)
            {
                entry->Failure = failure;
                entry->State.store(RuntimeResourceState::Failed, std::memory_order_release);
                ++mImpl->Stats.Failures;
            }
            else
            {
                entry->Resource = std::move(loadedResource);
                entry->ResidentBytes = residentBytes;
                entry->State.store(RuntimeResourceState::Ready, std::memory_order_release);
                if (mImpl->Stats.Resident != std::numeric_limits<uint64_t>::max())
                    ++mImpl->Stats.Resident;
                AddResidentBytesChecked(mImpl->Stats, residentBytes);
            }

            FinishActiveLoad(*mImpl, entry);
        }
        entry->Completion.notify_all();
        mImpl->LoadsCompleted.notify_all();

        if (bDiscarded)
        {
            throw std::runtime_error(
                "Runtime resource load was cancelled before publication: " + key.Path);
        }
        if (failure)
            std::rethrow_exception(failure);
        return { entry, entry->Resource.get() };
    }

    bool RuntimeResourceRegistry::QueueAsyncErased(
        const RuntimeResourceKey& key,
        std::type_index resourceType,
        std::function<std::shared_ptr<void>(std::stop_token)> cpuLoader,
        std::function<std::shared_ptr<void>(std::shared_ptr<void>)> mainThreadFinalizer,
        size_t residentBytes,
        uint64_t pinGroupId)
    {
        ValidateRuntimeResourceKey(key);
        if (!cpuLoader || !mainThreadFinalizer)
            throw std::invalid_argument("Async runtime resource loaders cannot be empty.");

        bool bQueued = false;
        {
            std::lock_guard lock(mImpl->Mutex);
            if (!mImpl->bInitialized || !mImpl->bAcceptingRequests)
                throw std::logic_error("Runtime resource registry is not accepting requests.");

            auto groupIt = mImpl->Groups.end();
            if (pinGroupId != 0)
            {
                groupIt = mImpl->Groups.find(pinGroupId);
                if (groupIt == mImpl->Groups.end())
                    throw std::logic_error("Pinned async runtime resource group is no longer valid.");
            }

            Impl::Slot& slot = mImpl->Slots[key];
            std::shared_ptr<Detail::RuntimeResourceEntry> entry = slot.Current;
            if (!entry)
                entry = CreateEntry(*mImpl, slot, key);

            const RuntimeResourceState state = entry->State.load(std::memory_order_acquire);
            if (entry->ResourceType != std::type_index(typeid(void)) && entry->ResourceType != resourceType)
                throw std::invalid_argument("Runtime resource key was requested with a different resource type.");

            bool bInsertedPin = false;
            if (groupIt != mImpl->Groups.end())
                (void)PinGroupEntryStrong(groupIt->second, entry, &bInsertedPin);

            if (state == RuntimeResourceState::Unloaded)
            {
                Impl::CpuJob job;
                job.Entry = entry;
                job.Loader = std::move(cpuLoader);
                job.Finalizer = std::move(mainThreadFinalizer);
                job.ResidentBytes = residentBytes;
                try
                {
                    mImpl->CpuJobs.emplace_back(std::move(job));
                }
                catch (...)
                {
                    if (bInsertedPin)
                        UnpinGroupEntryStrong(groupIt->second, entry);
                    throw;
                }

                entry->ResourceType = resourceType;
                entry->Failure = nullptr;
                entry->bCancelRequested = false;
                entry->LoadStopSource = std::stop_source{};
                entry->bLoadActive = true;
                entry->State.store(RuntimeResourceState::Queued, std::memory_order_release);
                ++mImpl->ActiveLoads;
                ++mImpl->Stats.PhysicalLoads;
                bQueued = true;
            }
            else if (state == RuntimeResourceState::Ready)
            {
                ++mImpl->Stats.CacheHits;
            }
            else if (state == RuntimeResourceState::Queued ||
                state == RuntimeResourceState::LoadingCpu ||
                state == RuntimeResourceState::PendingMainThread)
            {
                ++mImpl->Stats.InflightJoins;
            }
        }

        if (bQueued)
            mImpl->WorkAvailable.notify_one();
        return bQueued;
    }

    Detail::RuntimeResourceAcquireResult RuntimeResourceRegistry::TryAcquireReadyErased(
        const RuntimeResourceKey& key,
        std::type_index resourceType)
    {
        ValidateRuntimeResourceKey(key);
        std::lock_guard lock(mImpl->Mutex);
        if (!mImpl->bInitialized || !mImpl->bAcceptingRequests)
            return {};

        const auto slotIt = mImpl->Slots.find(key);
        if (slotIt == mImpl->Slots.end() || !slotIt->second.Current)
            return {};

        const std::shared_ptr<Detail::RuntimeResourceEntry>& entry = slotIt->second.Current;
        if (entry->ResourceType != std::type_index(typeid(void)) && entry->ResourceType != resourceType)
            throw std::invalid_argument("Runtime resource key was requested with a different resource type.");
        if (entry->State.load(std::memory_order_acquire) != RuntimeResourceState::Ready ||
            !entry->Resource)
        {
            return {};
        }

        ++mImpl->Stats.CacheHits;
        return { entry, entry->Resource.get() };
    }

    void RuntimeResourceRegistry::WorkerLoop(std::stop_token stopToken)
    {
        for (;;)
        {
            Impl::CpuJob job;
            std::stop_source loadStopSource;
            {
                std::unique_lock lock(mImpl->Mutex);
                mImpl->WorkAvailable.wait(lock, [this, stopToken]
                {
                    return mImpl->bStopWorkers || stopToken.stop_requested() ||
                        !mImpl->CpuJobs.empty();
                });
                if ((mImpl->bStopWorkers || stopToken.stop_requested()) && mImpl->CpuJobs.empty())
                    return;
                if (mImpl->CpuJobs.empty())
                    continue;

                job = std::move(mImpl->CpuJobs.front());
                mImpl->CpuJobs.pop_front();
                if (!job.Entry || job.Entry->bCancelRequested || job.Entry->bRetired ||
                    job.Entry->State.load(std::memory_order_acquire) != RuntimeResourceState::Queued)
                {
                    if (job.Entry)
                    {
                        job.Entry->State.store(RuntimeResourceState::EvictPending, std::memory_order_release);
                        FinishActiveLoad(*mImpl, job.Entry);
                        job.Entry->Completion.notify_all();
                        mImpl->LoadsCompleted.notify_all();
                        job.Entry.reset();
                    }
                    mImpl->DeferredMainThreadCpuJobs.emplace_back(std::move(job));
                    continue;
                }

                loadStopSource = job.Entry->LoadStopSource;
                job.Entry->LoaderThreadId = std::this_thread::get_id();
                job.Entry->State.store(RuntimeResourceState::LoadingCpu, std::memory_order_release);
            }
            job.Entry->Completion.notify_all();

            std::shared_ptr<void> payload;
            std::exception_ptr failure;
            std::stop_callback workerStopCallback(stopToken, [loadStopSource]() mutable
            {
                (void)loadStopSource.request_stop();
            });
            try
            {
                const ScopedRuntimeResourceCallback callbackScope(bInsideRuntimeResourceCpuLoader);
                payload = job.Loader(loadStopSource.get_token());
                if (!payload)
                    throw std::runtime_error("Async runtime resource CPU loader returned null.");
            }
            catch (...)
            {
                failure = std::current_exception();
            }

            bool bFinished = false;
            {
                std::lock_guard lock(mImpl->Mutex);
                job.Entry->LoaderThreadId = {};
                if (job.Entry->bCancelRequested || job.Entry->bRetired ||
                    mImpl->bStopWorkers || stopToken.stop_requested() ||
                    loadStopSource.stop_requested())
                {
                    if (payload)
                        mImpl->DeferredMainThreadReleases.emplace_back(std::move(payload));
                    job.Entry->State.store(RuntimeResourceState::EvictPending, std::memory_order_release);
                    FinishActiveLoad(*mImpl, job.Entry);
                    bFinished = true;
                }
                else if (failure)
                {
                    job.Entry->Failure = failure;
                    job.Entry->State.store(RuntimeResourceState::Failed, std::memory_order_release);
                    ++mImpl->Stats.Failures;
                    FinishActiveLoad(*mImpl, job.Entry);
                    bFinished = true;
                }
                else
                {
                    Impl::MainThreadWork work;
                    work.Entry = job.Entry;
                    work.Payload = std::move(payload);
                    work.Finalizer = std::move(job.Finalizer);
                    work.ResidentBytes = job.ResidentBytes;
                    try
                    {
                        mImpl->MainThreadWorkQueue.emplace_back(std::move(work));
                        job.Entry->State.store(
                            RuntimeResourceState::PendingMainThread, std::memory_order_release);
                    }
                    catch (...)
                    {
                        if (work.Payload)
                            mImpl->DeferredMainThreadReleases.emplace_back(std::move(work.Payload));
                        job.Finalizer = std::move(work.Finalizer);
                        job.Entry->Failure = std::current_exception();
                        job.Entry->State.store(RuntimeResourceState::Failed, std::memory_order_release);
                        ++mImpl->Stats.Failures;
                        FinishActiveLoad(*mImpl, job.Entry);
                        bFinished = true;
                    }
                }
                job.Entry->Completion.notify_all();
                if (bFinished)
                    mImpl->LoadsCompleted.notify_all();
                job.Entry.reset();
                mImpl->DeferredMainThreadCpuJobs.emplace_back(std::move(job));
            }
        }
    }

    size_t RuntimeResourceRegistry::PumpMainThreadWork(
        std::chrono::nanoseconds timeBudget,
        size_t maxItems)
    {
        return PumpMainThreadWorkInternal(timeBudget, maxItems, {});
    }

    size_t RuntimeResourceRegistry::PumpMainThreadWorkInternal(
        std::chrono::nanoseconds timeBudget,
        size_t maxItems,
        const std::shared_ptr<Detail::RuntimeResourceEntry>& targetEntry)
    {
        const auto startTime = std::chrono::steady_clock::now();
        const bool bHasTimeLimit = timeBudget > std::chrono::nanoseconds::zero();
        size_t processedItems = 0;
        std::vector<std::shared_ptr<void>> releaseList;
        std::deque<Impl::CpuJob> deferredCpuJobs;

        {
            std::lock_guard lock(mImpl->Mutex);
            VerifyInitThread(*mImpl, "RuntimeResourceRegistry::PumpMainThreadWork");
            if (bInsideRuntimeResourceFinalizer)
                throw std::logic_error("Runtime resource finalizers cannot recursively pump main-thread work.");
            releaseList.swap(mImpl->DeferredMainThreadReleases);
            deferredCpuJobs.swap(mImpl->DeferredMainThreadCpuJobs);
        }

        while (processedItems < maxItems)
        {
            if (processedItems > 0 && bHasTimeLimit &&
                std::chrono::steady_clock::now() - startTime >= timeBudget)
            {
                break;
            }

            Impl::MainThreadWork work;
            bool bRunFinalizer = false;
            {
                std::lock_guard lock(mImpl->Mutex);
                auto workIt = mImpl->MainThreadWorkQueue.begin();
                if (targetEntry)
                {
                    workIt = std::find_if(
                        mImpl->MainThreadWorkQueue.begin(),
                        mImpl->MainThreadWorkQueue.end(),
                        [&targetEntry](const Impl::MainThreadWork& candidate)
                        {
                            return candidate.Entry == targetEntry;
                        });
                }
                if (workIt == mImpl->MainThreadWorkQueue.end())
                    break;

                work = std::move(*workIt);
                mImpl->MainThreadWorkQueue.erase(workIt);
                bRunFinalizer = work.Entry && !work.Entry->bRetired &&
                    !work.Entry->bCancelRequested && mImpl->bAcceptingRequests &&
                    work.Entry->State.load(std::memory_order_acquire) ==
                        RuntimeResourceState::PendingMainThread;
                if (bRunFinalizer)
                    work.Entry->FinalizerThreadId = std::this_thread::get_id();
            }

            std::shared_ptr<void> resource;
            std::exception_ptr failure;
            if (bRunFinalizer)
            {
                try
                {
                    const ScopedRuntimeResourceCallback callbackScope(bInsideRuntimeResourceFinalizer);
                    resource = work.Finalizer(std::move(work.Payload));
                    if (!resource)
                        throw std::runtime_error("Async runtime resource finalizer returned null.");
                }
                catch (...)
                {
                    failure = std::current_exception();
                }
            }

            {
                std::lock_guard lock(mImpl->Mutex);
                if (work.Entry)
                {
                    work.Entry->FinalizerThreadId = {};
                    const bool bDiscard = !bRunFinalizer || work.Entry->bRetired ||
                        work.Entry->bCancelRequested || !mImpl->bAcceptingRequests;
                    if (bDiscard)
                    {
                        if (resource)
                            releaseList.emplace_back(std::move(resource));
                        work.Entry->State.store(
                            RuntimeResourceState::EvictPending, std::memory_order_release);
                    }
                    else if (failure)
                    {
                        work.Entry->Failure = failure;
                        work.Entry->State.store(RuntimeResourceState::Failed, std::memory_order_release);
                        ++mImpl->Stats.Failures;
                    }
                    else
                    {
                        work.Entry->Resource = std::move(resource);
                        work.Entry->ResidentBytes = work.ResidentBytes;
                        work.Entry->State.store(RuntimeResourceState::Ready, std::memory_order_release);
                        if (mImpl->Stats.Resident != std::numeric_limits<uint64_t>::max())
                            ++mImpl->Stats.Resident;
                        AddResidentBytesChecked(mImpl->Stats, work.ResidentBytes);
                    }
                    FinishActiveLoad(*mImpl, work.Entry);
                }
            }
            if (work.Entry)
                work.Entry->Completion.notify_all();
            mImpl->LoadsCompleted.notify_all();
            ++processedItems;
        }

        releaseList.clear();
        deferredCpuJobs.clear();
        return processedItems;
    }

    RuntimeResourceState RuntimeResourceRegistry::GetState(const RuntimeResourceKey& key) const
    {
        ValidateRuntimeResourceKey(key);
        std::lock_guard lock(mImpl->Mutex);
        const auto slotIt = mImpl->Slots.find(key);
        if (slotIt == mImpl->Slots.end() || !slotIt->second.Current)
            return RuntimeResourceState::Unloaded;
        return slotIt->second.Current->State.load(std::memory_order_acquire);
    }

    void RuntimeResourceRegistry::RethrowFailure(const RuntimeResourceKey& key) const
    {
        ValidateRuntimeResourceKey(key);
        std::exception_ptr failure;
        bool bFailedWithoutException = false;
        {
            std::lock_guard lock(mImpl->Mutex);
            const auto slotIt = mImpl->Slots.find(key);
            if (slotIt == mImpl->Slots.end() || !slotIt->second.Current ||
                slotIt->second.Current->State.load(std::memory_order_acquire) !=
                    RuntimeResourceState::Failed)
            {
                return;
            }

            failure = slotIt->second.Current->Failure;
            bFailedWithoutException = !failure;
        }

        if (failure)
            std::rethrow_exception(failure);
        if (bFailedWithoutException)
            throw std::runtime_error("Runtime resource load failed without an exception.");
    }

    bool RuntimeResourceRegistry::Retry(const RuntimeResourceKey& key)
    {
        ValidateRuntimeResourceKey(key);
        std::lock_guard lock(mImpl->Mutex);
        if (!mImpl->bInitialized || !mImpl->bAcceptingRequests)
            return false;

        const auto slotIt = mImpl->Slots.find(key);
        if (slotIt == mImpl->Slots.end() || !slotIt->second.Current ||
            slotIt->second.Current->State.load(std::memory_order_acquire) != RuntimeResourceState::Failed)
        {
            return false;
        }

        slotIt->second.Current->bRetired = true;
        mImpl->RetiredEntries.emplace_back(std::move(slotIt->second.Current));
        return true;
    }

    bool RuntimeResourceRegistry::Invalidate(const RuntimeResourceKey& key)
    {
        ValidateRuntimeResourceKey(key);
        std::shared_ptr<Detail::RuntimeResourceEntry> entry;
        std::stop_source loadStopSource;
        bool bFinishedLoad = false;
        {
            std::lock_guard lock(mImpl->Mutex);
            VerifyInitThread(*mImpl, "RuntimeResourceRegistry::Invalidate");
            if (!mImpl->bInitialized || !mImpl->bAcceptingRequests)
                return false;

            const auto slotIt = mImpl->Slots.find(key);
            if (slotIt == mImpl->Slots.end() || !slotIt->second.Current)
                return false;

            entry = std::move(slotIt->second.Current);
            entry->bRetired = true;
            entry->bCancelRequested = true;
            loadStopSource = entry->LoadStopSource;
            mImpl->RetiredEntries.emplace_back(entry);

            const auto cpuEnd = std::remove_if(
                mImpl->CpuJobs.begin(),
                mImpl->CpuJobs.end(),
                [&entry](const Impl::CpuJob& job)
                {
                    return job.Entry == entry;
                });
            if (cpuEnd != mImpl->CpuJobs.end())
            {
                mImpl->CpuJobs.erase(cpuEnd, mImpl->CpuJobs.end());
                entry->State.store(RuntimeResourceState::EvictPending, std::memory_order_release);
                FinishActiveLoad(*mImpl, entry);
                bFinishedLoad = true;
            }

            const auto mainEnd = std::remove_if(
                mImpl->MainThreadWorkQueue.begin(),
                mImpl->MainThreadWorkQueue.end(),
                [this, &entry](Impl::MainThreadWork& work)
                {
                    if (work.Entry != entry)
                        return false;
                    if (work.Payload)
                        mImpl->DeferredMainThreadReleases.emplace_back(std::move(work.Payload));
                    return true;
                });
            if (mainEnd != mImpl->MainThreadWorkQueue.end())
            {
                mImpl->MainThreadWorkQueue.erase(mainEnd, mImpl->MainThreadWorkQueue.end());
                entry->State.store(RuntimeResourceState::EvictPending, std::memory_order_release);
                FinishActiveLoad(*mImpl, entry);
                bFinishedLoad = true;
            }
        }
        (void)loadStopSource.request_stop();
        entry->Completion.notify_all();
        if (bFinishedLoad)
            mImpl->LoadsCompleted.notify_all();
        mImpl->WorkAvailable.notify_all();
        return true;
    }

    RuntimeResourceGroup RuntimeResourceRegistry::CreateGroup()
    {
        std::lock_guard lock(mImpl->Mutex);
        if (!mImpl->bInitialized || !mImpl->bAcceptingRequests)
            throw std::logic_error("Runtime resource registry is not accepting groups.");

        const uint64_t id = mImpl->NextGroupId++;
        mImpl->Groups.emplace(id, Impl::Group{});
        return RuntimeResourceGroup(this, id);
    }

    bool RuntimeResourceRegistry::PinGroupEntry(
        uint64_t groupId,
        const std::shared_ptr<Detail::RuntimeResourceEntry>& entry)
    {
        if (!entry)
            return false;

        std::lock_guard lock(mImpl->Mutex);
        const auto groupIt = mImpl->Groups.find(groupId);
        if (groupIt == mImpl->Groups.end() || entry->OwnerId != mImpl->RegistryId)
            return false;

        return PinGroupEntryStrong(groupIt->second, entry);
    }

    bool RuntimeResourceRegistry::PinGroupKey(uint64_t groupId, const RuntimeResourceKey& key)
    {
        ValidateRuntimeResourceKey(key);
        std::lock_guard lock(mImpl->Mutex);
        const auto groupIt = mImpl->Groups.find(groupId);
        if (groupIt == mImpl->Groups.end() || !mImpl->bAcceptingRequests)
            return false;

        Impl::Slot& slot = mImpl->Slots[key];
        std::shared_ptr<Detail::RuntimeResourceEntry> entry = slot.Current;
        if (!entry)
            entry = CreateEntry(*mImpl, slot, key);

        return PinGroupEntryStrong(groupIt->second, entry);
    }

    RuntimeResourceGroupProgress RuntimeResourceRegistry::GetGroupProgress(uint64_t groupId) const
    {
        std::lock_guard lock(mImpl->Mutex);
        RuntimeResourceGroupProgress progress;
        const auto groupIt = mImpl->Groups.find(groupId);
        if (groupIt == mImpl->Groups.end())
            return progress;

        progress.Total = groupIt->second.Entries.size();
        for (const std::shared_ptr<Detail::RuntimeResourceEntry>& entry : groupIt->second.Entries)
        {
            switch (entry->State.load(std::memory_order_acquire))
            {
                case RuntimeResourceState::Ready:
                    ++progress.Ready;
                    break;
                case RuntimeResourceState::Failed:
                    ++progress.Failed;
                    break;
                case RuntimeResourceState::Queued:
                case RuntimeResourceState::LoadingCpu:
                case RuntimeResourceState::PendingMainThread:
                    ++progress.Loading;
                    break;
                case RuntimeResourceState::Unloaded:
                case RuntimeResourceState::EvictPending:
                    ++progress.Unloaded;
                    break;
            }
        }

        if (progress.Total != 0)
        {
            progress.Ratio = static_cast<float>(progress.Ready + progress.Failed) /
                static_cast<float>(progress.Total);
        }
        return progress;
    }

    void RuntimeResourceRegistry::ReleaseGroup(uint64_t groupId) noexcept
    {
        std::lock_guard lock(mImpl->Mutex);
        const auto groupIt = mImpl->Groups.find(groupId);
        if (groupIt == mImpl->Groups.end())
            return;

        for (const std::shared_ptr<Detail::RuntimeResourceEntry>& entry : groupIt->second.Entries)
            entry->PinCount.fetch_sub(1, std::memory_order_release);
        mImpl->Groups.erase(groupIt);
    }

    size_t RuntimeResourceRegistry::CollectUnused()
    {
        std::vector<std::shared_ptr<void>> releaseList;
        std::deque<Impl::CpuJob> deferredCpuJobs;
        size_t evictedResources = 0;

        {
            std::lock_guard lock(mImpl->Mutex);
            VerifyInitThread(*mImpl, "RuntimeResourceRegistry::CollectUnused");
            releaseList.swap(mImpl->DeferredMainThreadReleases);
            deferredCpuJobs.swap(mImpl->DeferredMainThreadCpuJobs);

            for (auto& [key, slot] : mImpl->Slots)
            {
                (void)key;
                const std::shared_ptr<Detail::RuntimeResourceEntry>& entry = slot.Current;
                if (!entry || entry->LeaseCount.load(std::memory_order_acquire) != 0 ||
                    entry->PinCount.load(std::memory_order_acquire) != 0 || entry.use_count() != 1)
                {
                    continue;
                }

                const RuntimeResourceState state = entry->State.load(std::memory_order_acquire);
                if (state == RuntimeResourceState::Ready)
                {
                    MoveResourceToReleaseList(*mImpl, entry, releaseList);
                    ++evictedResources;
                    slot.Current.reset();
                }
                else if (state == RuntimeResourceState::Unloaded)
                {
                    slot.Current.reset();
                }
            }

            const auto newEnd = std::remove_if(
                mImpl->RetiredEntries.begin(),
                mImpl->RetiredEntries.end(),
                [this, &releaseList, &evictedResources](
                    const std::shared_ptr<Detail::RuntimeResourceEntry>& entry)
                {
                    if (entry->LeaseCount.load(std::memory_order_acquire) != 0 ||
                        entry->PinCount.load(std::memory_order_acquire) != 0 || entry.use_count() != 1)
                    {
                        return false;
                    }

                    const RuntimeResourceState state = entry->State.load(std::memory_order_acquire);
                    if (state == RuntimeResourceState::LoadingCpu ||
                        state == RuntimeResourceState::Queued ||
                        state == RuntimeResourceState::PendingMainThread)
                    {
                        return false;
                    }
                    if (entry->Resource)
                    {
                        MoveResourceToReleaseList(*mImpl, entry, releaseList);
                        ++evictedResources;
                    }
                    return true;
                });
            mImpl->RetiredEntries.erase(newEnd, mImpl->RetiredEntries.end());
        }

        releaseList.clear();
        deferredCpuJobs.clear();
        return evictedResources;
    }

    void RuntimeResourceRegistry::Shutdown()
    {
        std::vector<std::shared_ptr<void>> releaseList;
        std::vector<std::jthread> workers;
        std::deque<Impl::CpuJob> cancelledCpuJobs;
        std::deque<Impl::MainThreadWork> cancelledMainWork;
        std::deque<Impl::CpuJob> deferredCpuJobs;
        std::vector<std::stop_source> loadStopSources;

        {
            std::unique_lock lock(mImpl->Mutex);
            VerifyInitThread(*mImpl, "RuntimeResourceRegistry::Shutdown");
            if (bInsideRuntimeResourceCpuLoader || bInsideRuntimeResourceFinalizer)
                throw std::logic_error("Runtime resource loaders and finalizers cannot shut down the registry.");
            if (!mImpl->bInitialized)
                return;

            mImpl->bAcceptingRequests = false;
            mImpl->bStopWorkers = true;

            const auto requestCancel = [&loadStopSources](
                const std::shared_ptr<Detail::RuntimeResourceEntry>& entry)
            {
                if (entry && !IsLoadTerminal(entry->State.load(std::memory_order_acquire)))
                {
                    entry->bCancelRequested = true;
                    loadStopSources.emplace_back(entry->LoadStopSource);
                }
            };
            for (const auto& [key, slot] : mImpl->Slots)
            {
                (void)key;
                requestCancel(slot.Current);
            }
            for (const std::shared_ptr<Detail::RuntimeResourceEntry>& entry : mImpl->RetiredEntries)
                requestCancel(entry);

            cancelledCpuJobs.swap(mImpl->CpuJobs);
            for (Impl::CpuJob& job : cancelledCpuJobs)
            {
                if (!job.Entry)
                    continue;
                job.Entry->bCancelRequested = true;
                job.Entry->State.store(RuntimeResourceState::EvictPending, std::memory_order_release);
                FinishActiveLoad(*mImpl, job.Entry);
                job.Entry->Completion.notify_all();
            }

            cancelledMainWork.swap(mImpl->MainThreadWorkQueue);
            for (Impl::MainThreadWork& work : cancelledMainWork)
            {
                if (!work.Entry)
                    continue;
                work.Entry->bCancelRequested = true;
                work.Entry->State.store(RuntimeResourceState::EvictPending, std::memory_order_release);
                FinishActiveLoad(*mImpl, work.Entry);
                work.Entry->Completion.notify_all();
            }

            releaseList.swap(mImpl->DeferredMainThreadReleases);
            deferredCpuJobs.swap(mImpl->DeferredMainThreadCpuJobs);
            workers = std::move(mImpl->Workers);

            for (auto& [id, group] : mImpl->Groups)
            {
                (void)id;
                for (const std::shared_ptr<Detail::RuntimeResourceEntry>& entry : group.Entries)
                    entry->PinCount.fetch_sub(1, std::memory_order_release);
            }
            mImpl->Groups.clear();
        }

        for (std::stop_source& loadStopSource : loadStopSources)
            (void)loadStopSource.request_stop();
        for (std::jthread& worker : workers)
            worker.request_stop();
        mImpl->WorkAvailable.notify_all();
        workers.clear();
        cancelledCpuJobs.clear();
        cancelledMainWork.clear();

        {
            std::unique_lock lock(mImpl->Mutex);
            mImpl->LoadsCompleted.wait(lock, [this]
            {
                return mImpl->ActiveLoads == 0;
            });
            for (std::shared_ptr<void>& deferredRelease : mImpl->DeferredMainThreadReleases)
                releaseList.emplace_back(std::move(deferredRelease));
            mImpl->DeferredMainThreadReleases.clear();
            for (Impl::CpuJob& deferredJob : mImpl->DeferredMainThreadCpuJobs)
                deferredCpuJobs.emplace_back(std::move(deferredJob));
            mImpl->DeferredMainThreadCpuJobs.clear();

            bool bHasOutstandingLease = false;
            for (const auto& [key, slot] : mImpl->Slots)
            {
                (void)key;
                const std::shared_ptr<Detail::RuntimeResourceEntry>& entry = slot.Current;
                if (entry && entry->Resource &&
                    (entry->LeaseCount.load(std::memory_order_acquire) != 0 || entry.use_count() != 1))
                {
                    bHasOutstandingLease = true;
                    break;
                }
            }
            if (!bHasOutstandingLease)
            {
                for (const std::shared_ptr<Detail::RuntimeResourceEntry>& entry : mImpl->RetiredEntries)
                {
                    if (entry && entry->Resource &&
                        (entry->LeaseCount.load(std::memory_order_acquire) != 0 || entry.use_count() != 1))
                    {
                        bHasOutstandingLease = true;
                        break;
                    }
                }
            }
            if (bHasOutstandingLease)
            {
                throw std::logic_error(
                    "Runtime resource Shutdown requires all handles and shared leases to be released.");
            }

            for (auto& [key, slot] : mImpl->Slots)
            {
                (void)key;
                if (!slot.Current)
                    continue;
                slot.Current->bRetired = true;
                mImpl->RetiredEntries.emplace_back(std::move(slot.Current));
            }
            mImpl->Slots.clear();

            const auto newEnd = std::remove_if(
                mImpl->RetiredEntries.begin(),
                mImpl->RetiredEntries.end(),
                [this, &releaseList](const std::shared_ptr<Detail::RuntimeResourceEntry>& entry)
                {
                    if (entry->LeaseCount.load(std::memory_order_acquire) != 0 ||
                        entry->PinCount.load(std::memory_order_acquire) != 0 || entry.use_count() != 1)
                    {
                        return false;
                    }
                    MoveResourceToReleaseList(*mImpl, entry, releaseList);
                    return true;
                });
            mImpl->RetiredEntries.erase(newEnd, mImpl->RetiredEntries.end());
            mImpl->bInitialized = false;
        }

        releaseList.clear();
        deferredCpuJobs.clear();
    }

    RuntimeResourceStats RuntimeResourceRegistry::GetStats() const noexcept
    {
        std::lock_guard lock(mImpl->Mutex);
        return mImpl->Stats;
    }

    RuntimeResourceGroup::RuntimeResourceGroup(RuntimeResourceRegistry* registry, uint64_t id) noexcept
        : mRegistry(registry), mId(id)
    {
    }

    RuntimeResourceGroup::RuntimeResourceGroup(RuntimeResourceGroup&& other) noexcept
        : mRegistry(std::exchange(other.mRegistry, nullptr)), mId(std::exchange(other.mId, 0))
    {
    }

    RuntimeResourceGroup& RuntimeResourceGroup::operator=(RuntimeResourceGroup&& other) noexcept
    {
        if (this == &other)
            return *this;

        Release();
        mRegistry = std::exchange(other.mRegistry, nullptr);
        mId = std::exchange(other.mId, 0);
        return *this;
    }

    RuntimeResourceGroup::~RuntimeResourceGroup()
    {
        Release();
    }

    bool RuntimeResourceGroup::Pin(const RuntimeResourceKey& key)
    {
        return mRegistry != nullptr && mId != 0 && mRegistry->PinGroupKey(mId, key);
    }

    RuntimeResourceGroupProgress RuntimeResourceGroup::Progress() const
    {
        if (mRegistry == nullptr || mId == 0)
            return {};
        return mRegistry->GetGroupProgress(mId);
    }

    void RuntimeResourceGroup::Release() noexcept
    {
        if (mRegistry == nullptr || mId == 0)
            return;

        mRegistry->ReleaseGroup(mId);
        mRegistry = nullptr;
        mId = 0;
    }

    RuntimeResourceGroup::operator bool() const noexcept
    {
        return mRegistry != nullptr && mId != 0;
    }
}
