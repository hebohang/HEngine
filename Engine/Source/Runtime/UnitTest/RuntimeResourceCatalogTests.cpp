#include "Hepch.h"

#include "Runtime/UnitTest/RuntimeResourceCatalogTests.h"
#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceCatalog.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <memory>
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
        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        RuntimeCatalogDependency MakeDependency(
            std::string path,
            RuntimeCatalogDependencyKind kind,
            RuntimeCatalogDependencySource source)
        {
            RuntimeCatalogDependency dependency;
            dependency.Path = std::move(path);
            dependency.Kind = kind;
            dependency.Source = source;
            return dependency;
        }

        RuntimeResourceCatalogEntry MakeEntry(
            std::string path,
            RuntimeCatalogResourceType type,
            uint32_t cookFormatVersion,
            std::vector<RuntimeCatalogDependency> dependencies,
            std::vector<std::string> tags,
            uint64_t cpuBytes,
            uint64_t gpuBytes)
        {
            RuntimeResourceCatalogEntry entry;
            entry.Path = std::move(path);
            entry.Type = type;
            entry.CookFormatVersion = cookFormatVersion;
            entry.Dependencies = std::move(dependencies);
            entry.Tags = std::move(tags);
            entry.CpuBytes = cpuBytes;
            entry.GpuBytes = gpuBytes;
            return entry;
        }

        std::vector<RuntimeResourceCatalogEntry> MakeEntries()
        {
            return {
                MakeEntry(
                    "Assets/A.he",
                    RuntimeCatalogResourceType::Scene,
                    1,
                    {
                        MakeDependency(
                            "Assets/B.hmesh",
                            RuntimeCatalogDependencyKind::HardRuntime,
                            RuntimeCatalogDependencySource::Structured),
                        MakeDependency(
                            "Assets/C.hprefab",
                            RuntimeCatalogDependencyKind::SoftRuntime,
                            RuntimeCatalogDependencySource::Structured),
                        MakeDependency(
                            "Assets/D.bin",
                            RuntimeCatalogDependencyKind::PackagingOnly,
                            RuntimeCatalogDependencySource::Heuristic)
                    },
                    { "Warmup:Boot", "Zone:A" },
                    101,
                    202),
                MakeEntry(
                    "Assets/B.hmesh",
                    RuntimeCatalogResourceType::Mesh,
                    1,
                    {
                        MakeDependency(
                            "Assets/A.he",
                            RuntimeCatalogDependencyKind::HardRuntime,
                            RuntimeCatalogDependencySource::Structured)
                    },
                    { "Warmup:Boot" },
                    303,
                    404),
                MakeEntry(
                    "Assets/C.hprefab",
                    RuntimeCatalogResourceType::Prefab,
                    1,
                    {},
                    { "Warmup:Boot" },
                    505,
                    0),
                MakeEntry(
                    "Assets/D.bin",
                    RuntimeCatalogResourceType::Binary,
                    0,
                    {},
                    {},
                    606,
                    0)
            };
        }

        std::shared_ptr<const RuntimeResourceCatalogSnapshot> MakeSnapshot(
            std::vector<RuntimeResourceCatalogEntry> entries)
        {
            const std::string hash = RuntimeResourceCatalogSnapshot::ComputeRegistryHash(entries);
            return RuntimeResourceCatalogSnapshot::Create(std::move(entries), hash);
        }

        void RequireCreateRejected(
            std::vector<RuntimeResourceCatalogEntry> entries,
            std::string_view message)
        {
            bool bRejected = false;
            try
            {
                const std::string hash = RuntimeResourceCatalogSnapshot::ComputeRegistryHash(entries);
                (void)RuntimeResourceCatalogSnapshot::Create(std::move(entries), hash);
            }
            catch (const std::invalid_argument&)
            {
                bRejected = true;
            }
            Require(bRejected, message);
        }

        void RequirePaths(
            const std::vector<const RuntimeResourceCatalogEntry*>& entries,
            std::initializer_list<std::string_view> expectedPaths,
            std::string_view message)
        {
            if (entries.size() != expectedPaths.size())
                throw std::runtime_error(std::string(message));

            size_t index = 0;
            for (std::string_view expectedPath : expectedPaths)
            {
                if (entries[index]->Path != expectedPath)
                    throw std::runtime_error(std::string(message));
                ++index;
            }
        }

        void VerifyLookupAndExpansion()
        {
            std::vector<RuntimeResourceCatalogEntry> entries = MakeEntries();
            std::swap(entries[0], entries[2]);
            const auto snapshot = MakeSnapshot(std::move(entries));

            const RuntimeResourceCatalogEntry* alias = snapshot->Find("assets\\./A.he");
            Require(alias != nullptr && alias->Path == "Assets/A.he",
                "A normalized case/separator alias did not find its catalog entry.");
            Require(snapshot->Find("../Assets/A.he") == nullptr,
                "A traversing catalog lookup was accepted.");

            const auto firstTagged = snapshot->FindByTag("Warmup:Boot");
            const auto secondTagged = snapshot->FindByTag("Warmup:Boot");
            RequirePaths(
                firstTagged,
                { "Assets/A.he", "Assets/B.hmesh", "Assets/C.hprefab" },
                "Tag lookup results were not in stable canonical path order.");
            RequirePaths(
                secondTagged,
                { "Assets/A.he", "Assets/B.hmesh", "Assets/C.hprefab" },
                "Repeated tag lookup changed its canonical order.");

            const auto hardClosure = snapshot->Expand(
                "ASSETS/A.he",
                RuntimeCatalogExpand::HardRuntime);
            RequirePaths(
                hardClosure,
                { "Assets/A.he", "Assets/B.hmesh" },
                "Hard-runtime expansion included the wrong closure or failed to terminate a cycle.");

            const auto runtimeClosure = snapshot->Expand(
                "Assets/A.he",
                RuntimeCatalogExpand::Runtime);
            RequirePaths(
                runtimeClosure,
                { "Assets/A.he", "Assets/B.hmesh", "Assets/C.hprefab" },
                "Runtime expansion did not add soft dependencies or incorrectly added packaging-only dependencies.");
        }

        void VerifyRegistryHash()
        {
            const std::vector<RuntimeResourceCatalogEntry> entries = MakeEntries();
            const std::string hash = RuntimeResourceCatalogSnapshot::ComputeRegistryHash(entries);
            Require(hash == "28d50072d67a7101",
                "The catalog registry hash no longer matches the v1 canonical FNV-1a fixture.");
            Require(RuntimeResourceCatalogSnapshot::ComputeRegistryHash(MakeEntries()) == hash,
                "Equivalent catalog values produced a non-deterministic registry hash.");

            const auto requireChanged = [&](std::vector<RuntimeResourceCatalogEntry> changed, std::string_view field)
            {
                if (RuntimeResourceCatalogSnapshot::ComputeRegistryHash(changed) == hash)
                    throw std::runtime_error("Registry hash ignored catalog field: " + std::string(field));
            };

            auto changed = entries;
            changed[0].Path = "Assets/AA.he";
            requireChanged(std::move(changed), "Path");
            changed = entries;
            changed[0].Type = RuntimeCatalogResourceType::Prefab;
            requireChanged(std::move(changed), "Type");
            changed = entries;
            ++changed[0].CookFormatVersion;
            requireChanged(std::move(changed), "CookFormatVersion");
            changed = entries;
            changed[0].Dependencies[0].Path = "Assets/D.bin";
            requireChanged(std::move(changed), "Dependency.Path");
            changed = entries;
            changed[0].Dependencies[0].Kind = RuntimeCatalogDependencyKind::SoftRuntime;
            requireChanged(std::move(changed), "Dependency.Kind");
            changed = entries;
            changed[0].Dependencies[0].Source = RuntimeCatalogDependencySource::Heuristic;
            requireChanged(std::move(changed), "Dependency.Source");
            changed = entries;
            changed[0].Tags[0] = "Warmup:Menu";
            requireChanged(std::move(changed), "Tags");
            changed = entries;
            ++changed[0].CpuBytes;
            requireChanged(std::move(changed), "CpuBytes");
            changed = entries;
            ++changed[0].GpuBytes;
            requireChanged(std::move(changed), "GpuBytes");

            changed = entries;
            changed[0].CacheKey = "transient-entry-key";
            changed[0].Dependencies[0].CacheKey = "transient-dependency-key";
            Require(RuntimeResourceCatalogSnapshot::ComputeRegistryHash(changed) == hash,
                "Transient runtime cache keys leaked into the packaged registry hash.");

            bool bMismatchRejected = false;
            try
            {
                (void)RuntimeResourceCatalogSnapshot::Create(entries, "0000000000000000");
            }
            catch (const std::invalid_argument&)
            {
                bMismatchRejected = true;
            }
            Require(bMismatchRejected, "A snapshot accepted a registry hash that did not match its fields.");
        }

        void VerifyContractRejections()
        {
            auto entries = MakeEntries();
            entries[0].Type = static_cast<RuntimeCatalogResourceType>(255);
            RequireCreateRejected(std::move(entries),
                "A snapshot accepted an invalid resource type enum.");

            entries = MakeEntries();
            entries[0].Dependencies[0].Kind = static_cast<RuntimeCatalogDependencyKind>(255);
            RequireCreateRejected(std::move(entries),
                "A snapshot accepted an invalid dependency-kind enum.");

            entries = MakeEntries();
            entries[0].Dependencies[0].Source = static_cast<RuntimeCatalogDependencySource>(255);
            RequireCreateRejected(std::move(entries),
                "A snapshot accepted an invalid dependency-source enum.");

            RequireCreateRejected(
                { MakeEntry(
                    "Assets/CONIN$/Bad.bin",
                    RuntimeCatalogResourceType::Binary,
                    0,
                    {},
                    {},
                    1,
                    0) },
                "A snapshot accepted a Windows-reserved project path.");

            entries = MakeEntries();
            entries[0].GpuBytes = RuntimeResourceCatalogLimits::MaxResourceEstimate + 1;
            RequireCreateRejected(std::move(entries),
                "A snapshot accepted an oversized per-resource estimate.");

            std::vector<RuntimeResourceCatalogEntry> aggregateEntries;
            for (std::uint64_t index = 0; index < 17; ++index)
            {
                aggregateEntries.push_back(MakeEntry(
                    "Assets/Aggregate" + std::to_string(index) + ".bin",
                    RuntimeCatalogResourceType::Binary,
                    0,
                    {},
                    {},
                    RuntimeResourceCatalogLimits::MaxResourceEstimate,
                    0));
            }
            RequireCreateRejected(std::move(aggregateEntries),
                "A snapshot accepted an oversized aggregate resource estimate.");

            entries = MakeEntries();
            entries[0].Dependencies.assign(
                RuntimeResourceCatalogLimits::MaxDependenciesPerEntry + 1,
                MakeDependency(
                    "Assets/B.hmesh",
                    RuntimeCatalogDependencyKind::HardRuntime,
                    RuntimeCatalogDependencySource::Structured));
            RequireCreateRejected(std::move(entries),
                "A snapshot accepted too many per-entry dependencies.");

            entries = MakeEntries();
            entries[0].Tags.assign(RuntimeResourceCatalogLimits::MaxTagsPerEntry + 1, "Tag");
            RequireCreateRejected(std::move(entries),
                "A snapshot accepted too many per-entry tags.");
        }

        void VerifyAtomicInstallAndClear()
        {
            RuntimeResourceCatalog& catalog = RuntimeResourceCatalog::GetInstance();
            catalog.Clear();
            const auto empty = RuntimeResourceCatalogSnapshot::Empty();
            const auto first = MakeSnapshot(MakeEntries());
            std::vector<RuntimeResourceCatalogEntry> secondEntries = MakeEntries();
            secondEntries[0].CpuBytes += 1000;
            const auto second = MakeSnapshot(std::move(secondEntries));

            catalog.Install(first);
            Require(catalog.GetSnapshot() == first,
                "Installing a catalog snapshot did not publish that exact immutable snapshot.");

            constexpr size_t ReaderCount = 4;
            std::atomic_size_t readyCount = 0;
            std::atomic_bool bStart = false;
            std::atomic_bool bStop = false;
            std::atomic_bool bObservedInvalidSnapshot = false;
            std::vector<std::thread> readers;
            readers.reserve(ReaderCount);
            for (size_t index = 0; index < ReaderCount; ++index)
            {
                readers.emplace_back([&]
                {
                    readyCount.fetch_add(1, std::memory_order_release);
                    while (!bStart.load(std::memory_order_acquire))
                        std::this_thread::yield();

                    do
                    {
                        const auto observed = catalog.GetSnapshot();
                        if (!observed || (observed != empty && observed != first && observed != second))
                            bObservedInvalidSnapshot.store(true, std::memory_order_relaxed);
                    }
                    while (!bStop.load(std::memory_order_acquire));
                });
            }

            while (readyCount.load(std::memory_order_acquire) != ReaderCount)
                std::this_thread::yield();
            bStart.store(true, std::memory_order_release);
            for (size_t iteration = 0; iteration < 4096; ++iteration)
                catalog.Install(iteration % 2 == 0 ? first : second);
            catalog.Clear();
            bStop.store(true, std::memory_order_release);
            for (std::thread& reader : readers)
                reader.join();

            Require(!bObservedInvalidSnapshot.load(std::memory_order_relaxed),
                "A concurrent reader observed a torn or unknown catalog snapshot.");
            Require(catalog.GetSnapshot() == empty && catalog.GetSnapshot()->IsEmpty(),
                "Clearing the catalog did not atomically restore the shared empty snapshot.");

            catalog.Install(nullptr);
            Require(catalog.GetSnapshot() == empty,
                "Installing a null snapshot did not publish the shared empty snapshot.");
        }
    }

    void RuntimeResourceCatalogContract()
    {
        RuntimeCatalogResourceType collisionType = RuntimeCatalogResourceType::Data;
        Require(TryParseRuntimeCatalogResourceType("Collision", collisionType) &&
            collisionType == RuntimeCatalogResourceType::Collision &&
            std::string_view(ToString(collisionType)) == "Collision",
            "Packaged collision manifest type failed its parser/formatter round trip.");
        const auto collisionCatalog = MakeSnapshot({MakeEntry(
            "Assets/Collision/Room.hcollision", collisionType, 1, {}, {}, 1024, 0)});
        const auto* collision = collisionCatalog->Find("Assets/Collision/Room.hcollision");
        Require(collision && collision->Type == RuntimeCatalogResourceType::Collision &&
            collision->CookFormatVersion == 1,
            "Collision was rejected or relabeled by catalog validation/install.");
        VerifyLookupAndExpansion();
        VerifyRegistryHash();
        VerifyContractRejections();
        VerifyAtomicInstallAndClear();
    }
}
