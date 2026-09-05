#include "Hepch.h"

#include "Runtime/UnitTest/RuntimeResourceRegistryTests.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeResourceRegistry.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace HEngine::Tests
{
    namespace
    {
        using namespace std::chrono_literals;

        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        class ThreadStartGate
        {
        public:
            explicit ThreadStartGate(size_t participantCount)
                : mParticipantCount(participantCount)
            {
            }

            void ArriveAndWait()
            {
                std::unique_lock lock(mMutex);
                ++mReadyCount;
                mCondition.notify_all();
                mCondition.wait(lock, [this]
                {
                    return mReleased;
                });
            }

            [[nodiscard]] bool ReleaseWhenReady(std::chrono::milliseconds timeout)
            {
                std::unique_lock lock(mMutex);
                const bool bAllReady = mCondition.wait_for(lock, timeout, [this]
                {
                    return mReadyCount == mParticipantCount;
                });
                mReleased = true;
                lock.unlock();
                mCondition.notify_all();
                return bAllReady;
            }

        private:
            const size_t mParticipantCount;
            std::mutex mMutex;
            std::condition_variable mCondition;
            size_t mReadyCount = 0;
            bool mReleased = false;
        };

        class LoaderGate
        {
        public:
            void EnterAndWait()
            {
                std::unique_lock lock(mMutex);
                mEntered = true;
                mCondition.notify_all();
                mCondition.wait(lock, [this]
                {
                    return mReleased;
                });
            }

            [[nodiscard]] bool WaitUntilEntered(std::chrono::milliseconds timeout)
            {
                std::unique_lock lock(mMutex);
                return mCondition.wait_for(lock, timeout, [this]
                {
                    return mEntered;
                });
            }

            void Release()
            {
                {
                    std::lock_guard lock(mMutex);
                    mReleased = true;
                }
                mCondition.notify_all();
            }

        private:
            std::mutex mMutex;
            std::condition_variable mCondition;
            bool mEntered = false;
            bool mReleased = false;
        };

        template<typename Predicate>
        [[nodiscard]] bool WaitUntil(Predicate&& predicate, std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (predicate())
                    return true;
                std::this_thread::sleep_for(1ms);
            }
            return predicate();
        }

        struct ThreadTrackedResource
        {
            ThreadTrackedResource(
                int value,
                std::thread::id* destructionThread,
                size_t* destructionCount)
                : Value(value), DestructionThread(destructionThread),
                  DestructionCount(destructionCount)
            {
            }

            ~ThreadTrackedResource()
            {
                *DestructionThread = std::this_thread::get_id();
                ++*DestructionCount;
            }

            int Value = 0;
            std::thread::id* DestructionThread = nullptr;
            size_t* DestructionCount = nullptr;
        };

        struct ThreadTrackedPayload
        {
            ThreadTrackedPayload(
                int value,
                std::thread::id* destructionThread,
                size_t* destructionCount)
                : Value(value), DestructionThread(destructionThread),
                  DestructionCount(destructionCount)
            {
            }

            ThreadTrackedPayload(const ThreadTrackedPayload&) = delete;
            ThreadTrackedPayload& operator=(const ThreadTrackedPayload&) = delete;

            ThreadTrackedPayload(ThreadTrackedPayload&& other) noexcept
                : Value(other.Value),
                  DestructionThread(std::exchange(other.DestructionThread, nullptr)),
                  DestructionCount(std::exchange(other.DestructionCount, nullptr))
            {
            }

            ~ThreadTrackedPayload()
            {
                if (DestructionThread == nullptr || DestructionCount == nullptr)
                    return;
                *DestructionThread = std::this_thread::get_id();
                ++*DestructionCount;
            }

            int Value = 0;
            std::thread::id* DestructionThread = nullptr;
            size_t* DestructionCount = nullptr;
        };

        [[nodiscard]] bool IsPublicationCancellation(const std::exception_ptr& failure)
        {
            if (!failure)
                return false;

            try
            {
                std::rethrow_exception(failure);
            }
            catch (const std::runtime_error& exception)
            {
                return std::string_view(exception.what()).find("cancelled before publication") !=
                    std::string_view::npos;
            }
            catch (...)
            {
                return false;
            }
        }

        void VerifyCanonicalKeys()
        {
            const RuntimeResourceKey mixedSeparators = RuntimeResourceKey::FromProjectPath(
                RuntimeResourceKind::Mesh,
                "Assets\\Models/./Hero/Hero.hmesh",
                "lod0");
            const RuntimeResourceKey normalizedCaseAlias = RuntimeResourceKey::FromProjectPath(
                RuntimeResourceKind::Mesh,
                "assets/models/hero/hero.hmesh",
                "lod0");

            Require(mixedSeparators == normalizedCaseAlias,
                "Equivalent Windows resource paths did not produce one registry key.");
            Require(RuntimeResourceKeyHash{}(mixedSeparators) ==
                    RuntimeResourceKeyHash{}(normalizedCaseAlias),
                "Equivalent resource keys produced different hashes.");
            Require(mixedSeparators != RuntimeResourceKey::FromProjectPath(
                    RuntimeResourceKind::Texture,
                    "assets/models/hero/hero.hmesh",
                    "lod0"),
                "Resource kind was omitted from registry identity.");
            Require(mixedSeparators != RuntimeResourceKey::FromProjectPath(
                    RuntimeResourceKind::Mesh,
                    "assets/models/hero/hero.hmesh",
                    "lod1"),
                "Resource variant was omitted from registry identity.");
        }

        void VerifyConcurrentDeduplication(RuntimeResourceRegistry& registry)
        {
            constexpr size_t kWorkerCount = 100;
            const RuntimeResourceKey firstAlias = RuntimeResourceKey::FromProjectPath(
                RuntimeResourceKind::Mesh,
                "Assets\\Concurrent/./Shared.hmesh");
            const RuntimeResourceKey secondAlias = RuntimeResourceKey::FromProjectPath(
                RuntimeResourceKind::Mesh,
                "assets/concurrent/shared.hmesh");
            const RuntimeResourceStats before = registry.GetStats();

            ThreadStartGate startGate(kWorkerCount);
            LoaderGate loaderGate;
            std::atomic_size_t loaderCalls = 0;
            std::vector<RuntimeResourceHandle<int>> handles(kWorkerCount);
            std::vector<std::exception_ptr> failures(kWorkerCount);
            std::vector<std::thread> workers;
            workers.reserve(kWorkerCount);

            for (size_t index = 0; index < kWorkerCount; ++index)
            {
                workers.emplace_back([&, index]
                {
                    startGate.ArriveAndWait();
                    try
                    {
                        const RuntimeResourceKey& key = index % 2 == 0 ? firstAlias : secondAlias;
                        handles[index] = registry.AcquireBlocking<int>(key, [&]
                        {
                            loaderCalls.fetch_add(1, std::memory_order_relaxed);
                            loaderGate.EnterAndWait();
                            return std::make_shared<int>(42);
                        }, sizeof(int));
                    }
                    catch (...)
                    {
                        failures[index] = std::current_exception();
                    }
                });
            }

            const bool bAllWorkersReady = startGate.ReleaseWhenReady(3s);
            const bool bLoaderEntered = loaderGate.WaitUntilEntered(3s);
            const bool bAllRequestsJoined = bLoaderEntered && WaitUntil([&]
            {
                return registry.GetStats().InflightJoins - before.InflightJoins == kWorkerCount - 1;
            }, 3s);
            loaderGate.Release();
            for (std::thread& worker : workers)
                worker.join();

            Require(bAllWorkersReady, "Concurrent registry workers did not reach the start gate.");
            Require(bLoaderEntered, "The physical registry loader did not start.");
            Require(bAllRequestsJoined, "Not all concurrent requests joined the in-flight load.");
            Require(loaderCalls.load(std::memory_order_relaxed) == 1,
                "Concurrent aliases triggered more than one physical load.");
            for (size_t index = 0; index < kWorkerCount; ++index)
            {
                Require(failures[index] == nullptr, "A concurrent registry request unexpectedly failed.");
                Require(handles[index] && *handles[index] == 42,
                    "A concurrent registry handle returned the wrong resource.");
            }

            const RuntimeResourceStats afterJoin = registry.GetStats();
            Require(afterJoin.PhysicalLoads - before.PhysicalLoads == 1,
                "Concurrent request accounting did not record exactly one physical load.");
            Require(afterJoin.InflightJoins - before.InflightJoins == kWorkerCount - 1,
                "Concurrent request accounting did not record every in-flight join.");

            bool bCacheLoaderCalled = false;
            RuntimeResourceHandle<int> cacheHit = registry.AcquireBlocking<int>(secondAlias, [&]
            {
                bCacheLoaderCalled = true;
                return std::make_shared<int>(99);
            });
            Require(!bCacheLoaderCalled && cacheHit && *cacheHit == 42,
                "A ready registry entry did not serve a cache hit.");
            std::shared_ptr<int> sharedLease = cacheHit.GetShared();
            Require(sharedLease && *sharedLease == 42,
                "A cache-hit handle did not create a valid shared lease.");

            for (RuntimeResourceHandle<int>& handle : handles)
                handle.Reset();
            cacheHit.Reset();
            Require(registry.CollectUnused() == 0,
                "A ready resource was evicted while its shared lease was alive.");
            sharedLease.reset();
            Require(registry.CollectUnused() == 1,
                "The first load plus cache hits did not evict exactly one physical resource.");
            Require(registry.CollectUnused() == 0,
                "A physical resource was evicted more than once.");
        }

        void VerifyFailureFanOutAndRetry(RuntimeResourceRegistry& registry)
        {
            constexpr size_t kWorkerCount = 16;
            const RuntimeResourceKey key = RuntimeResourceKey::FromProjectPath(
                RuntimeResourceKind::Prefab,
                "Assets/Concurrent/Failure.hprefab");
            const RuntimeResourceStats before = registry.GetStats();

            ThreadStartGate startGate(kWorkerCount);
            LoaderGate loaderGate;
            std::atomic_size_t loaderCalls = 0;
            std::vector<std::string> errors(kWorkerCount);
            std::vector<std::thread> workers;
            workers.reserve(kWorkerCount);

            for (size_t index = 0; index < kWorkerCount; ++index)
            {
                workers.emplace_back([&, index]
                {
                    startGate.ArriveAndWait();
                    try
                    {
                        (void)registry.AcquireBlocking<int>(key, [&]() -> std::shared_ptr<int>
                        {
                            loaderCalls.fetch_add(1, std::memory_order_relaxed);
                            loaderGate.EnterAndWait();
                            throw std::runtime_error("expected registry load failure");
                        });
                        errors[index] = "load unexpectedly succeeded";
                    }
                    catch (const std::exception& exception)
                    {
                        errors[index] = exception.what();
                    }
                    catch (...)
                    {
                        errors[index] = "unexpected non-standard exception";
                    }
                });
            }

            const bool bAllWorkersReady = startGate.ReleaseWhenReady(3s);
            const bool bLoaderEntered = loaderGate.WaitUntilEntered(3s);
            const bool bAllRequestsJoined = bLoaderEntered && WaitUntil([&]
            {
                return registry.GetStats().InflightJoins - before.InflightJoins == kWorkerCount - 1;
            }, 3s);
            loaderGate.Release();
            for (std::thread& worker : workers)
                worker.join();

            Require(bAllWorkersReady, "Failure fan-out workers did not reach the start gate.");
            Require(bLoaderEntered, "The failing physical registry loader did not start.");
            Require(bAllRequestsJoined, "Not all failure requests joined the in-flight load.");
            Require(loaderCalls.load(std::memory_order_relaxed) == 1,
                "A failed in-flight request triggered duplicate physical loads.");
            for (const std::string& error : errors)
            {
                Require(error == "expected registry load failure",
                    "A joined failure did not receive the cached loader exception.");
            }
            Require(registry.GetState(key) == RuntimeResourceState::Failed,
                "A failed registry entry did not retain Failed state.");
            Require(registry.Retry(key), "A failed registry entry could not be retried.");

            RuntimeResourceHandle<int> recovered = registry.AcquireBlocking<int>(key, [&]
            {
                loaderCalls.fetch_add(1, std::memory_order_relaxed);
                return std::make_shared<int>(77);
            });
            Require(recovered && *recovered == 77,
                "A retried registry load did not publish its recovered resource.");
            Require(recovered.GetGeneration() >= 2,
                "Retry reused the failed registry generation.");
            Require(loaderCalls.load(std::memory_order_relaxed) == 2,
                "Retry did not perform exactly one new physical load.");
            Require(!registry.Retry(key), "A ready registry entry was incorrectly accepted for retry.");

            recovered.Reset();
            Require(registry.CollectUnused() == 1,
                "The recovered registry generation was not evicted after release.");
        }

        void VerifyInvalidationGeneration(RuntimeResourceRegistry& registry)
        {
            const RuntimeResourceKey key = RuntimeResourceKey::FromProjectPath(
                RuntimeResourceKind::Texture,
                "Assets/Textures/Generation.png");
            RuntimeResourceHandle<int> oldHandle = registry.AcquireBlocking<int>(key, []
            {
                return std::make_shared<int>(1);
            });
            const uint64_t oldGeneration = oldHandle.GetGeneration();

            Require(registry.Invalidate(key), "A ready registry entry could not be invalidated.");
            Require(registry.GetState(key) == RuntimeResourceState::Unloaded,
                "Invalidation left the retired generation as current.");
            Require(oldHandle && *oldHandle == 1,
                "Invalidation destroyed a resource still referenced by an old handle.");

            RuntimeResourceHandle<int> newHandle = registry.AcquireBlocking<int>(key, []
            {
                return std::make_shared<int>(2);
            });
            Require(newHandle && *newHandle == 2,
                "Invalidation did not allow a fresh resource generation.");
            Require(newHandle.GetGeneration() > oldGeneration,
                "Invalidation did not advance the resource generation.");

            newHandle.Reset();
            Require(registry.CollectUnused() == 1,
                "The released current generation was not evicted.");
            Require(oldHandle && *oldHandle == 1,
                "Collecting the current generation invalidated a live retired handle.");
            oldHandle.Reset();
            Require(registry.CollectUnused() == 1,
                "The retired generation was not evicted after its final handle released.");
        }

        void VerifyBlockingInvalidationDiscard(RuntimeResourceRegistry& registry)
        {
            const RuntimeResourceKey key = RuntimeResourceKey::Builtin(
                RuntimeResourceKind::Generic,
                "BlockingInvalidationDiscard");
            const RuntimeResourceStats before = registry.GetStats();
            const std::thread::id initThread = std::this_thread::get_id();
            LoaderGate loaderGate;
            std::thread::id destructionThread;
            size_t destructionCount = 0;
            std::atomic_size_t joinedLoaderCalls = 0;
            std::exception_ptr physicalFailure;
            std::exception_ptr joinedFailure;

            std::thread physicalLoader([&]
            {
                try
                {
                    (void)registry.AcquireBlocking<ThreadTrackedResource>(key, [&]
                    {
                        loaderGate.EnterAndWait();
                        return std::make_shared<ThreadTrackedResource>(
                            31,
                            &destructionThread,
                            &destructionCount);
                    });
                }
                catch (...)
                {
                    physicalFailure = std::current_exception();
                }
            });

            const bool bLoaderEntered = loaderGate.WaitUntilEntered(3s);
            std::thread joinedLoader([&]
            {
                try
                {
                    (void)registry.AcquireBlocking<ThreadTrackedResource>(key, [&]
                    {
                        joinedLoaderCalls.fetch_add(1, std::memory_order_relaxed);
                        return std::make_shared<ThreadTrackedResource>(
                            32,
                            &destructionThread,
                            &destructionCount);
                    });
                }
                catch (...)
                {
                    joinedFailure = std::current_exception();
                }
            });

            const bool bJoinedInflight = bLoaderEntered && WaitUntil([&]
            {
                return registry.GetStats().InflightJoins > before.InflightJoins;
            }, 3s);
            const bool bInvalidated = registry.Invalidate(key);
            loaderGate.Release();
            physicalLoader.join();
            joinedLoader.join();

            Require(bLoaderEntered, "Blocking invalidation loader did not enter its publish window.");
            Require(bJoinedInflight, "Blocking invalidation request was not joined in flight.");
            Require(bInvalidated, "A running blocking generation could not be invalidated.");
            Require(joinedLoaderCalls.load(std::memory_order_relaxed) == 0,
                "Blocking invalidation started a second physical loader.");
            Require(IsPublicationCancellation(physicalFailure) &&
                    IsPublicationCancellation(joinedFailure),
                "Invalidated blocking callers did not both receive publication cancellation.");
            Require(registry.GetState(key) == RuntimeResourceState::Unloaded,
                "Invalidated blocking completion was republished as the current generation.");
            Require(registry.CollectUnused() == 0,
                "Discarding an unpublished blocking result counted it as a resident eviction.");
            Require(destructionCount == 1 && destructionThread == initThread,
                "Discarded blocking result was not destroyed exactly once on the Init thread.");
        }

        void VerifyGroupPinAndReleaseThread(RuntimeResourceRegistry& registry)
        {
            const std::thread::id initThread = std::this_thread::get_id();
            std::thread::id destructionThread;
            size_t destructionCount = 0;
            const RuntimeResourceKey key = RuntimeResourceKey::Builtin(
                RuntimeResourceKind::Generic,
                "RegistryThreadTrackedResource");

            RuntimeResourceHandle<ThreadTrackedResource> handle =
                registry.AcquireBlocking<ThreadTrackedResource>(key, [&]
                {
                    return std::make_shared<ThreadTrackedResource>(
                        5,
                        &destructionThread,
                        &destructionCount);
                });
            RuntimeResourceGroup group = registry.CreateGroup();
            Require(group.Pin(handle), "A registry group could not pin a ready handle.");
            Require(group.Pin(handle), "Pinning the same handle twice was not idempotent.");

            const RuntimeResourceGroupProgress progress = group.Progress();
            Require(progress.Total == 1 && progress.Ready == 1 && progress.Ratio == 1.0f,
                "A pinned ready group reported incorrect progress.");

            handle.Reset();
            Require(registry.CollectUnused() == 0 && destructionCount == 0,
                "A group-pinned resource was evicted after its handle released.");
            group.Release();
            Require(registry.CollectUnused() == 1,
                "A resource was not evicted after its final group pin released.");
            Require(destructionCount == 1 && destructionThread == initThread,
                "A registry resource was not destroyed exactly once on the Init thread.");
        }

        void VerifyQueueAsyncPinnedCollectRace(RuntimeResourceRegistry& registry)
        {
            const RuntimeResourceKey key = RuntimeResourceKey::Builtin(
                RuntimeResourceKind::Generic,
                "QueueAsyncPinnedCollectRace");
            RuntimeResourceHandle<int> initial = registry.AcquireBlocking<int>(key, []
            {
                return std::make_shared<int>(51);
            });
            initial.Reset();

            RuntimeResourceGroup group = registry.CreateGroup();
            ThreadStartGate startGate(1);
            std::atomic_size_t cpuCalls = 0;
            std::exception_ptr queueFailure;
            bool bQueued = false;
            std::thread queueThread([&]
            {
                startGate.ArriveAndWait();
                try
                {
                    bQueued = registry.QueueAsyncPinned<int, int>(
                        key,
                        group,
                        [&cpuCalls]
                        {
                            cpuCalls.fetch_add(1, std::memory_order_relaxed);
                            return 52;
                        },
                        [](int value)
                        {
                            return std::make_shared<int>(value);
                        });
                }
                catch (...)
                {
                    queueFailure = std::current_exception();
                }
            });

            const bool bQueueThreadReady = startGate.ReleaseWhenReady(3s);
            const size_t concurrentEvictions = registry.CollectUnused();
            queueThread.join();

            Require(bQueueThreadReady, "Pinned async queue did not reach its concurrent start gate.");
            Require(queueFailure == nullptr, "Pinned async queue failed during concurrent collection.");
            Require(concurrentEvictions <= 1,
                "Concurrent collection evicted more than the original ready generation.");
            RuntimeResourceGroupProgress progress = group.Progress();
            Require(progress.Total == 1 && progress.Unloaded == 0,
                "Pinned async queue lost its selected generation during concurrent collection.");

            if (bQueued)
            {
                Require(WaitUntil([&]
                {
                    return registry.GetState(key) == RuntimeResourceState::PendingMainThread;
                }, 3s), "Replacement pinned async generation did not reach PendingMainThread.");
                Require(registry.PumpMainThreadWork(0ns, 1) == 1,
                    "Replacement pinned async generation was not finalized.");
            }

            RuntimeResourceHandle<int> ready = registry.TryAcquireReady<int>(key);
            Require(ready && *ready == (bQueued ? 52 : 51),
                "Pinned async queue did not retain the selected ready generation.");
            Require(cpuCalls.load(std::memory_order_relaxed) == (bQueued ? 1u : 0u),
                "Pinned async queue executed an unexpected number of CPU loaders.");
            ready.Reset();
            Require(registry.CollectUnused() == 0,
                "A QueueAsyncPinned generation was evicted while its group was alive.");
            group.Release();
            Require(registry.CollectUnused() == 1,
                "A QueueAsyncPinned generation was not evicted after its group released.");
        }

        void VerifyBlockingDependencyGuard(RuntimeResourceRegistry& registry)
        {
            const RuntimeResourceKey readyKey = RuntimeResourceKey::Builtin(
                RuntimeResourceKind::Generic,
                "BlockingDependencyReady");
            const RuntimeResourceKey unloadedKey = RuntimeResourceKey::Builtin(
                RuntimeResourceKind::Generic,
                "BlockingDependencyUnloaded");
            const RuntimeResourceKey outerKey = RuntimeResourceKey::Builtin(
                RuntimeResourceKind::Generic,
                "BlockingDependencyOuter");

            RuntimeResourceHandle<int> readyHandle = registry.AcquireBlocking<int>(readyKey, []
            {
                return std::make_shared<int>(11);
            });
            bool bReadyDependencyJoined = false;
            bool bPhysicalDependencyRejected = false;
            RuntimeResourceHandle<int> outerHandle = registry.AcquireBlocking<int>(outerKey, [&]
            {
                RuntimeResourceHandle<int> readyDependency = registry.AcquireBlocking<int>(readyKey, []
                {
                    return std::make_shared<int>(-1);
                });
                bReadyDependencyJoined = readyDependency && *readyDependency == 11;

                try
                {
                    (void)registry.AcquireBlocking<int>(unloadedKey, []
                    {
                        return std::make_shared<int>(22);
                    });
                }
                catch (const std::logic_error&)
                {
                    bPhysicalDependencyRejected = true;
                }
                return std::make_shared<int>(33);
            });

            Require(bReadyDependencyJoined,
                "A blocking loader could not use an already-ready dependency.");
            Require(bPhysicalDependencyRejected,
                "A blocking loader was allowed to start a physical dependency load.");
            Require(outerHandle && *outerHandle == 33,
                "Blocking dependency rejection prevented the outer load from completing.");

            readyHandle.Reset();
            outerHandle.Reset();
            Require(registry.CollectUnused() == 2,
                "Blocking dependency guard resources were not evicted after release.");
        }

        void VerifyAsyncDedupAndMainThreadFinalize(RuntimeResourceRegistry& registry)
        {
            constexpr size_t kRequestCount = 100;
            const RuntimeResourceKey key = RuntimeResourceKey::Builtin(
                RuntimeResourceKind::Generic,
                "AsyncDedup");
            const RuntimeResourceStats before = registry.GetStats();
            const std::thread::id initThread = std::this_thread::get_id();
            std::atomic_size_t cpuCalls = 0;
            size_t finalizeCalls = 0;
            std::thread::id cpuThread;
            std::thread::id finalizeThread;

            for (size_t index = 0; index < kRequestCount; ++index)
            {
                const bool bQueued = registry.QueueAsync<int, int>(
                    key,
                    [&]
                    {
                        cpuThread = std::this_thread::get_id();
                        cpuCalls.fetch_add(1, std::memory_order_relaxed);
                        return 41;
                    },
                    [&](int value)
                    {
                        ++finalizeCalls;
                        finalizeThread = std::this_thread::get_id();
                        return std::make_shared<int>(value + 1);
                    },
                    sizeof(int));
                Require(bQueued == (index == 0),
                    "Only the first async request should queue physical work.");
            }

            Require(WaitUntil([&]
            {
                return registry.GetState(key) == RuntimeResourceState::PendingMainThread;
            }, 3s), "Async CPU work did not reach PendingMainThread.");
            Require(!registry.TryAcquireReady<int>(key),
                "TryAcquireReady exposed a resource before main-thread finalize.");
            Require(registry.PumpMainThreadWork(0ns, 1) == 1,
                "Main-thread pump did not finalize exactly one async resource.");

            RuntimeResourceHandle<int> handle = registry.TryAcquireReady<int>(key);
            Require(handle && *handle == 42,
                "TryAcquireReady did not return the finalized async resource.");
            Require(cpuCalls.load(std::memory_order_relaxed) == 1 && finalizeCalls == 1,
                "Async same-key requests duplicated CPU or finalize work.");
            Require(cpuThread != initThread && finalizeThread == initThread,
                "Async CPU/finalize stages ran on the wrong threads.");

            const RuntimeResourceStats after = registry.GetStats();
            Require(after.PhysicalLoads - before.PhysicalLoads == 1,
                "Async dedup did not record exactly one physical load.");
            Require(after.InflightJoins - before.InflightJoins == kRequestCount - 1,
                "Async dedup did not record every joined request.");

            handle.Reset();
            Require(registry.CollectUnused() == 1,
                "Finalized async resource was not evicted after release.");
        }

        void VerifyAsyncPumpLimits(RuntimeResourceRegistry& registry)
        {
            std::vector<RuntimeResourceKey> keys;
            keys.reserve(3);
            std::atomic_size_t finalizeCalls = 0;
            for (size_t index = 0; index < 3; ++index)
            {
                keys.emplace_back(RuntimeResourceKey::Builtin(
                    RuntimeResourceKind::Generic,
                    "AsyncPumpLimit" + std::to_string(index)));
                Require(registry.QueueAsync<int, int>(
                    keys.back(),
                    [index]
                    {
                        return static_cast<int>(index);
                    },
                    [&finalizeCalls](int value)
                    {
                        std::this_thread::sleep_for(3ms);
                        finalizeCalls.fetch_add(1, std::memory_order_relaxed);
                        return std::make_shared<int>(value);
                    }), "A distinct async pump-limit request was not queued.");
            }

            Require(WaitUntil([&]
            {
                for (const RuntimeResourceKey& key : keys)
                {
                    if (registry.GetState(key) != RuntimeResourceState::PendingMainThread)
                        return false;
                }
                return true;
            }, 3s), "Pump-limit resources did not all reach PendingMainThread.");

            Require(registry.PumpMainThreadWork(0ns, 0) == 0,
                "Pump ignored a zero maxItems limit.");
            Require(registry.PumpMainThreadWork(0ns, 1) == 1,
                "Pump ignored maxItems=1.");
            Require(registry.PumpMainThreadWork(1ms, 10) == 1,
                "Pump did not stop after exceeding its time budget.");
            Require(registry.PumpMainThreadWork(0ns, 10) == 1,
                "Pump did not drain the final queued completion.");
            Require(finalizeCalls.load(std::memory_order_relaxed) == 3,
                "Pump limits lost or duplicated a main-thread finalizer.");

            for (const RuntimeResourceKey& key : keys)
            {
                RuntimeResourceHandle<int> handle = registry.TryAcquireReady<int>(key);
                Require(static_cast<bool>(handle),
                    "A pump-limit resource was not ready after draining.");
                handle.Reset();
            }
            Require(registry.CollectUnused() == 3,
                "Pump-limit resources were not evicted after release.");
        }

        void VerifyAsyncFailureRetryAndBlockingJoin(RuntimeResourceRegistry& registry)
        {
            const RuntimeResourceKey failureKey = RuntimeResourceKey::Builtin(
                RuntimeResourceKind::Generic,
                "AsyncFailureRetry");
            std::atomic_size_t cpuCalls = 0;
            Require(registry.QueueAsync<int, int>(
                failureKey,
                [&]() -> int
                {
                    cpuCalls.fetch_add(1, std::memory_order_relaxed);
                    throw std::runtime_error("expected async CPU failure");
                },
                [](int value)
                {
                    return std::make_shared<int>(value);
                }), "Initial failing async load was not queued.");
            Require(WaitUntil([&]
            {
                return registry.GetState(failureKey) == RuntimeResourceState::Failed;
            }, 3s), "Async CPU failure did not reach Failed state.");
            bool bObservedCachedFailure = false;
            try
            {
                registry.RethrowFailure(failureKey);
            }
            catch (const std::runtime_error& exception)
            {
                bObservedCachedFailure =
                    std::string_view(exception.what()) == "expected async CPU failure";
            }
            Require(bObservedCachedFailure,
                "The async Failed state did not preserve its original exception.");
            Require(registry.Retry(failureKey), "Async Failed state could not be retried.");
            Require(registry.QueueAsync<int, int>(
                failureKey,
                [&]
                {
                    cpuCalls.fetch_add(1, std::memory_order_relaxed);
                    return 73;
                },
                [](int value)
                {
                    return std::make_shared<int>(value);
                }), "Retried async load was not queued.");
            Require(WaitUntil([&]
            {
                return registry.GetState(failureKey) == RuntimeResourceState::PendingMainThread;
            }, 3s), "Retried async load did not reach PendingMainThread.");

            RuntimeResourceHandle<int> recovered = registry.AcquireBlocking<int>(failureKey, []
            {
                return std::make_shared<int>(-1);
            });
            Require(recovered && *recovered == 73,
                "Init-thread AcquireBlocking did not pump and join PendingMainThread work.");
            Require(cpuCalls.load(std::memory_order_relaxed) == 2,
                "Async retry performed an unexpected number of CPU loads.");
            recovered.Reset();
            Require(registry.CollectUnused() == 1,
                "Retried async resource was not evicted after release.");
        }

        void VerifyAsyncInvalidation(RuntimeResourceRegistry& registry)
        {
            const RuntimeResourceKey key = RuntimeResourceKey::Builtin(
                RuntimeResourceKind::Generic,
                "AsyncInvalidation");
            std::atomic_size_t oldFinalizers = 0;
            std::atomic_size_t newFinalizers = 0;

            Require(registry.QueueAsync<int, int>(
                key,
                []
                {
                    return 1;
                },
                [&oldFinalizers](int value)
                {
                    oldFinalizers.fetch_add(1, std::memory_order_relaxed);
                    return std::make_shared<int>(value);
                }), "Old async generation was not queued.");
            Require(WaitUntil([&]
            {
                return registry.GetState(key) == RuntimeResourceState::PendingMainThread;
            }, 3s), "Old async generation did not reach PendingMainThread.");
            Require(registry.Invalidate(key), "Pending async generation could not be invalidated.");

            Require(registry.QueueAsync<int, int>(
                key,
                []
                {
                    return 2;
                },
                [&newFinalizers](int value)
                {
                    newFinalizers.fetch_add(1, std::memory_order_relaxed);
                    return std::make_shared<int>(value);
                }), "New async generation was not queued after invalidation.");
            Require(WaitUntil([&]
            {
                return registry.GetState(key) == RuntimeResourceState::PendingMainThread;
            }, 3s), "New async generation did not reach PendingMainThread.");
            Require(registry.PumpMainThreadWork(0ns, 1) == 1,
                "New async generation was not finalized.");

            RuntimeResourceHandle<int> handle = registry.TryAcquireReady<int>(key);
            Require(handle && *handle == 2,
                "Stale async completion replaced the new generation.");
            Require(oldFinalizers.load(std::memory_order_relaxed) == 0 &&
                    newFinalizers.load(std::memory_order_relaxed) == 1,
                "Invalidated pending work was finalized instead of discarded.");
            handle.Reset();
            Require(registry.CollectUnused() == 1,
                "New async generation was not evicted after release.");
        }

        void VerifyAsyncPumpCollectStability()
        {
            constexpr size_t kIterationCount = 32;
            RuntimeResourceRegistry registry;
            registry.Init(1);
            const RuntimeResourceKey key = RuntimeResourceKey::Builtin(
                RuntimeResourceKind::Generic,
                "AsyncPumpCollectStability");

            for (size_t index = 0; index < kIterationCount; ++index)
            {
                Require(registry.QueueAsync<int, int>(
                    key,
                    [index]
                    {
                        return static_cast<int>(index);
                    },
                    [](int value)
                    {
                        return std::make_shared<int>(value);
                    }), "Repeated async pump/collect work was not queued.");
                Require(WaitUntil([&]
                {
                    return registry.GetState(key) == RuntimeResourceState::PendingMainThread;
                }, 3s), "Repeated async work did not reach PendingMainThread.");
                Require(registry.PumpMainThreadWork(0ns, 1) == 1,
                    "Repeated async work was not finalized exactly once.");
                Require(registry.CollectUnused() == 1,
                    "A single CollectUnused call did not evict a zero-handle async resource.");
            }

            registry.Shutdown();
        }

        void VerifyAsyncReadyWithoutHandleShutdown()
        {
            RuntimeResourceRegistry registry;
            registry.Init(1);
            const std::thread::id initThread = std::this_thread::get_id();
            std::thread::id cpuCallbackDestructionThread;
            std::thread::id finalizerCallbackDestructionThread;
            std::thread::id payloadDestructionThread;
            std::thread::id resourceDestructionThread;
            size_t cpuCallbackDestructionCount = 0;
            size_t finalizerCallbackDestructionCount = 0;
            size_t payloadDestructionCount = 0;
            size_t resourceDestructionCount = 0;

            auto cpuCallbackLifetime = std::make_shared<ThreadTrackedResource>(
                0,
                &cpuCallbackDestructionThread,
                &cpuCallbackDestructionCount);
            auto finalizerCallbackLifetime = std::make_shared<ThreadTrackedResource>(
                0,
                &finalizerCallbackDestructionThread,
                &finalizerCallbackDestructionCount);
            const RuntimeResourceKey key = RuntimeResourceKey::Builtin(
                RuntimeResourceKind::Generic,
                "AsyncReadyWithoutHandleShutdown");
            Require(registry.QueueAsync<ThreadTrackedResource, ThreadTrackedPayload>(
                key,
                [cpuCallbackLifetime, &payloadDestructionThread, &payloadDestructionCount]
                {
                    (void)cpuCallbackLifetime;
                    return ThreadTrackedPayload(
                        17,
                        &payloadDestructionThread,
                        &payloadDestructionCount);
                },
                [finalizerCallbackLifetime, &resourceDestructionThread, &resourceDestructionCount](
                    ThreadTrackedPayload payload)
                {
                    (void)finalizerCallbackLifetime;
                    return std::make_shared<ThreadTrackedResource>(
                        payload.Value,
                        &resourceDestructionThread,
                        &resourceDestructionCount);
                }), "Zero-handle async resource was not queued.");
            cpuCallbackLifetime.reset();
            finalizerCallbackLifetime.reset();

            Require(WaitUntil([&]
            {
                return registry.GetState(key) == RuntimeResourceState::PendingMainThread;
            }, 3s), "Zero-handle async resource did not reach PendingMainThread.");
            Require(registry.PumpMainThreadWork(0ns, 1) == 1,
                "Zero-handle async resource was not finalized.");
            Require(registry.GetState(key) == RuntimeResourceState::Ready,
                "Zero-handle async resource was not Ready before Shutdown.");
            Require(cpuCallbackDestructionCount == 1 &&
                    cpuCallbackDestructionThread == initThread,
                "Async CPU callback state was not destroyed on the Init thread.");
            Require(finalizerCallbackDestructionCount == 1 &&
                    finalizerCallbackDestructionThread == initThread,
                "Async finalizer callback state was not destroyed on the Init thread.");
            Require(payloadDestructionCount == 1 && payloadDestructionThread == initThread,
                "Finalized async payload was not destroyed on the Init thread.");

            registry.Shutdown();
            Require(!registry.IsInitialized(),
                "A Ready zero-handle async resource incorrectly blocked Shutdown.");
            Require(resourceDestructionCount == 1 && resourceDestructionThread == initThread,
                "Zero-handle async resource was not destroyed on the Init thread.");
        }

        void VerifyAsyncRunningInvalidationStop()
        {
            RuntimeResourceRegistry registry;
            registry.Init(1);
            const std::thread::id initThread = std::this_thread::get_id();
            std::atomic_bool loaderEntered = false;
            std::atomic_bool loaderExited = false;
            std::atomic_bool stopObserved = false;
            std::atomic_size_t finalizerCalls = 0;
            std::thread::id cpuCallbackDestructionThread;
            std::thread::id finalizerCallbackDestructionThread;
            std::thread::id payloadDestructionThread;
            size_t cpuCallbackDestructionCount = 0;
            size_t finalizerCallbackDestructionCount = 0;
            size_t payloadDestructionCount = 0;

            auto cpuCallbackLifetime = std::make_shared<ThreadTrackedResource>(
                0,
                &cpuCallbackDestructionThread,
                &cpuCallbackDestructionCount);
            auto finalizerCallbackLifetime = std::make_shared<ThreadTrackedResource>(
                0,
                &finalizerCallbackDestructionThread,
                &finalizerCallbackDestructionCount);
            const RuntimeResourceKey key = RuntimeResourceKey::Builtin(
                RuntimeResourceKind::Generic,
                "AsyncRunningInvalidationStop");
            Require(registry.QueueAsync<int, ThreadTrackedPayload>(
                key,
                [&, cpuCallbackLifetime](std::stop_token stopToken)
                {
                    (void)cpuCallbackLifetime;
                    loaderEntered.store(true, std::memory_order_release);
                    const auto deadline = std::chrono::steady_clock::now() + 2s;
                    while (!stopToken.stop_requested() &&
                        std::chrono::steady_clock::now() < deadline)
                    {
                        std::this_thread::yield();
                    }
                    stopObserved.store(stopToken.stop_requested(), std::memory_order_release);
                    loaderExited.store(true, std::memory_order_release);
                    return ThreadTrackedPayload(
                        23,
                        &payloadDestructionThread,
                        &payloadDestructionCount);
                },
                [&, finalizerCallbackLifetime](ThreadTrackedPayload payload)
                {
                    (void)finalizerCallbackLifetime;
                    finalizerCalls.fetch_add(1, std::memory_order_relaxed);
                    return std::make_shared<int>(payload.Value);
                }), "Running invalidation async resource was not queued.");
            cpuCallbackLifetime.reset();
            finalizerCallbackLifetime.reset();

            Require(WaitUntil([&]
            {
                return loaderEntered.load(std::memory_order_acquire) &&
                    registry.GetState(key) == RuntimeResourceState::LoadingCpu;
            }, 3s), "Running invalidation loader did not enter LoadingCpu.");
            Require(registry.Invalidate(key),
                "A LoadingCpu generation could not be invalidated.");
            Require(WaitUntil([&]
            {
                return loaderExited.load(std::memory_order_acquire);
            }, 3s), "Invalidated CPU loader did not exit after its stop request.");
            Require(stopObserved.load(std::memory_order_acquire),
                "Invalidate did not request the running generation stop token.");
            Require(WaitUntil([&]
            {
                (void)registry.PumpMainThreadWork(0ns, 0);
                return cpuCallbackDestructionCount == 1 &&
                    finalizerCallbackDestructionCount == 1 && payloadDestructionCount == 1;
            }, 3s), "Invalidated async callback or payload destruction was not drained.");
            Require(finalizerCalls.load(std::memory_order_relaxed) == 0,
                "Invalidated LoadingCpu work reached its main-thread finalizer.");
            Require(cpuCallbackDestructionThread == initThread &&
                    finalizerCallbackDestructionThread == initThread &&
                    payloadDestructionThread == initThread,
                "Invalidated async callbacks or payload were destroyed off the Init thread.");

            (void)registry.CollectUnused();
            registry.Shutdown();
        }

        void VerifyAsyncShutdownCancel()
        {
            RuntimeResourceRegistry registry;
            registry.Init(2);
            std::atomic_size_t runningLoaders = 0;
            std::atomic_size_t queuedLoaderCalls = 0;
            std::atomic_size_t finalizeCalls = 0;

            const RuntimeResourceKey pendingKey = RuntimeResourceKey::Builtin(
                RuntimeResourceKind::Generic,
                "AsyncShutdownPending");
            Require(registry.QueueAsync<int, int>(
                pendingKey,
                []
                {
                    return 1;
                },
                [&finalizeCalls](int value)
                {
                    finalizeCalls.fetch_add(1, std::memory_order_relaxed);
                    return std::make_shared<int>(value);
                }), "Shutdown pending work was not queued.");
            Require(WaitUntil([&]
            {
                return registry.GetState(pendingKey) == RuntimeResourceState::PendingMainThread;
            }, 3s), "Shutdown test did not establish pending main-thread work.");

            for (size_t index = 0; index < 2; ++index)
            {
                const RuntimeResourceKey key = RuntimeResourceKey::Builtin(
                    RuntimeResourceKind::Generic,
                    "AsyncShutdownRunning" + std::to_string(index));
                Require(registry.QueueAsync<int, int>(
                    key,
                    [&runningLoaders](std::stop_token stopToken)
                    {
                        runningLoaders.fetch_add(1, std::memory_order_relaxed);
                        while (!stopToken.stop_requested())
                            std::this_thread::yield();
                        return 2;
                    },
                    [&finalizeCalls](int value)
                    {
                        finalizeCalls.fetch_add(1, std::memory_order_relaxed);
                        return std::make_shared<int>(value);
                    }), "Shutdown running work was not queued.");
            }
            Require(WaitUntil([&]
            {
                return runningLoaders.load(std::memory_order_relaxed) == 2;
            }, 3s), "Shutdown test did not occupy both CPU workers.");

            const RuntimeResourceKey queuedKey = RuntimeResourceKey::Builtin(
                RuntimeResourceKind::Generic,
                "AsyncShutdownQueued");
            Require(registry.QueueAsync<int, int>(
                queuedKey,
                [&queuedLoaderCalls]
                {
                    queuedLoaderCalls.fetch_add(1, std::memory_order_relaxed);
                    return 3;
                },
                [&finalizeCalls](int value)
                {
                    finalizeCalls.fetch_add(1, std::memory_order_relaxed);
                    return std::make_shared<int>(value);
                }), "Shutdown queued work was not queued.");

            registry.Shutdown();
            Require(!registry.IsInitialized(), "Async Shutdown did not finish cleanly.");
            Require(queuedLoaderCalls.load(std::memory_order_relaxed) == 0,
                "Shutdown executed CPU work that was still queued.");
            Require(finalizeCalls.load(std::memory_order_relaxed) == 0,
                "Shutdown finalized cancelled pending/running async work.");
        }

        void VerifyBlockingShutdownDiscard()
        {
            RuntimeResourceRegistry registry;
            registry.Init(1);
            const std::thread::id initThread = std::this_thread::get_id();
            const RuntimeResourceKey key = RuntimeResourceKey::Builtin(
                RuntimeResourceKind::Generic,
                "BlockingShutdownDiscard");
            LoaderGate loaderGate;
            std::thread::id destructionThread;
            size_t destructionCount = 0;
            std::exception_ptr acquireFailure;
            std::exception_ptr observerFailure;
            std::atomic_bool beginShutdown = false;
            bool bObservedRequestRejection = false;

            std::thread loader([&]
            {
                try
                {
                    (void)registry.AcquireBlocking<ThreadTrackedResource>(key, [&]
                    {
                        loaderGate.EnterAndWait();
                        return std::make_shared<ThreadTrackedResource>(
                            61,
                            &destructionThread,
                            &destructionCount);
                    });
                }
                catch (...)
                {
                    acquireFailure = std::current_exception();
                }
            });
            const bool bLoaderEntered = loaderGate.WaitUntilEntered(3s);

            std::thread shutdownObserver([&]
            {
                while (!beginShutdown.load(std::memory_order_acquire))
                    std::this_thread::yield();

                const auto deadline = std::chrono::steady_clock::now() + 3s;
                while (std::chrono::steady_clock::now() < deadline)
                {
                    try
                    {
                        RuntimeResourceGroup probe = registry.CreateGroup();
                    }
                    catch (const std::logic_error&)
                    {
                        bObservedRequestRejection = true;
                        break;
                    }
                    catch (...)
                    {
                        observerFailure = std::current_exception();
                        break;
                    }
                    std::this_thread::yield();
                }
                loaderGate.Release();
            });

            bool bShutdownSucceeded = true;
            beginShutdown.store(true, std::memory_order_release);
            try
            {
                registry.Shutdown();
            }
            catch (...)
            {
                bShutdownSucceeded = false;
                loaderGate.Release();
            }
            shutdownObserver.join();
            loader.join();

            Require(bLoaderEntered, "Blocking Shutdown loader did not enter its publish window.");
            Require(observerFailure == nullptr && bObservedRequestRejection,
                "Blocking Shutdown test did not observe the request admission barrier.");
            Require(bShutdownSucceeded && !registry.IsInitialized(),
                "Shutdown did not finish after discarding a blocking completion.");
            Require(IsPublicationCancellation(acquireFailure),
                "Shutdown blocking caller received a published handle instead of cancellation.");
            Require(destructionCount == 1 && destructionThread == initThread,
                "Shutdown-discarded blocking result was not destroyed on the Init thread.");
            Require(registry.CollectUnused() == 0,
                "Shutdown-discarded blocking generation retained a resident resource.");
        }

        void VerifyShutdownRejectsRequests(RuntimeResourceRegistry& registry)
        {
            const RuntimeResourceKey liveKey = RuntimeResourceKey::Builtin(
                RuntimeResourceKind::Generic,
                "LiveDuringShutdown");
            RuntimeResourceHandle<int> liveHandle = registry.AcquireBlocking<int>(liveKey, []
            {
                return std::make_shared<int>(9);
            });
            std::shared_ptr<int> sharedLease = liveHandle.GetShared();
            liveHandle.Reset();

            bool bOutstandingLeaseRejected = false;
            try
            {
                registry.Shutdown();
            }
            catch (const std::logic_error&)
            {
                bOutstandingLeaseRejected = true;
            }
            Require(bOutstandingLeaseRejected && registry.IsInitialized(),
                "Shutdown did not reject an outstanding resource lease.");
            Require(sharedLease && *sharedLease == 9,
                "A rejected Shutdown invalidated its outstanding shared lease.");

            sharedLease.reset();
            Require(registry.CollectUnused() == 1,
                "A released Shutdown-blocking resource was not evicted on the Init thread.");
            registry.Shutdown();
            Require(!registry.IsInitialized(), "Registry remained initialized after Shutdown.");

            bool bAcquireRejected = false;
            try
            {
                const RuntimeResourceKey key = RuntimeResourceKey::Builtin(
                    RuntimeResourceKind::Generic,
                    "AfterShutdown");
                (void)registry.AcquireBlocking<int>(key, []
                {
                    return std::make_shared<int>(1);
                });
            }
            catch (const std::logic_error&)
            {
                bAcquireRejected = true;
            }
            Require(bAcquireRejected, "Registry accepted a resource request after Shutdown.");

            bool bGroupRejected = false;
            try
            {
                (void)registry.CreateGroup();
            }
            catch (const std::logic_error&)
            {
                bGroupRejected = true;
            }
            Require(bGroupRejected, "Registry accepted a resource group after Shutdown.");
        }
    }

    void RuntimeResourceRegistryLifecycle()
    {
        VerifyCanonicalKeys();

        RuntimeResourceRegistry registry;
        registry.Init();
        VerifyConcurrentDeduplication(registry);
        VerifyFailureFanOutAndRetry(registry);
        VerifyInvalidationGeneration(registry);
        VerifyBlockingInvalidationDiscard(registry);
        VerifyGroupPinAndReleaseThread(registry);
        VerifyQueueAsyncPinnedCollectRace(registry);
        VerifyBlockingDependencyGuard(registry);
        VerifyAsyncDedupAndMainThreadFinalize(registry);
        VerifyAsyncPumpLimits(registry);
        VerifyAsyncFailureRetryAndBlockingJoin(registry);
        VerifyAsyncInvalidation(registry);
        VerifyAsyncPumpCollectStability();
        VerifyAsyncReadyWithoutHandleShutdown();
        VerifyAsyncRunningInvalidationStop();
        VerifyAsyncShutdownCancel();
        VerifyBlockingShutdownDiscard();
        VerifyShutdownRejectsRequests(registry);
    }
}
