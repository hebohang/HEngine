#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace HEngine
{
    namespace RuntimeResourceCatalogLimits
    {
        inline constexpr std::uint32_t SchemaVersion = 1;
        inline constexpr std::string_view RegistryHashAlgorithm = "fnv1a64";
        // The schema version is already encoded in this domain. The top-level
        // ResourceCatalogVersion discriminator is therefore not hashed again.
        inline constexpr std::string_view RegistryHashDomain = "HEngine.ResourceRegistry.v1";
        inline constexpr std::uint64_t MaxEntries = 100000;
        inline constexpr std::uint64_t MaxDependencies = 1000000;
        inline constexpr std::uint64_t MaxTags = 1000000;
        inline constexpr std::size_t MaxDependenciesPerEntry = 4096;
        inline constexpr std::size_t MaxTagsPerEntry = 256;
        inline constexpr std::size_t MaxPathLength = 4096;
        inline constexpr std::size_t MaxTagLength = 256;
        inline constexpr std::uint64_t MaxResourceEstimate =
            1ull * 1024ull * 1024ull * 1024ull * 1024ull;
        inline constexpr std::uint64_t MaxCatalogEstimate = 16ull * MaxResourceEstimate;
    }

    enum class RuntimeCatalogResourceType : uint8_t
    {
        Scene,
        Prefab,
        Mesh,
        Material,
        Skeleton,
        AnimationClip,
        AnimationController,
        UiDocument,
        BehaviorTree,
        UiMarkup,
        Texture,
        ShaderSource,
        ShaderBinary,
        Script,
        Audio,
        Font,
        EffectPackage,
        Effect,
        MeshSource,
        Config,
        Binary,
        Data,
        // Append new types to preserve existing serialized numeric values and registry hashes.
        Collision
    };

    enum class RuntimeCatalogDependencyKind : uint8_t
    {
        HardRuntime,
        SoftRuntime,
        PackagingOnly
    };

    enum class RuntimeCatalogDependencySource : uint8_t
    {
        Structured,
        Heuristic
    };

    enum class RuntimeCatalogExpand : uint8_t
    {
        HardRuntime,
        Runtime
    };

    [[nodiscard]] const char* ToString(RuntimeCatalogResourceType type) noexcept;
    [[nodiscard]] const char* ToString(RuntimeCatalogDependencyKind kind) noexcept;
    [[nodiscard]] const char* ToString(RuntimeCatalogDependencySource source) noexcept;
    [[nodiscard]] bool TryParseRuntimeCatalogResourceType(
        std::string_view value,
        RuntimeCatalogResourceType& outType) noexcept;
    [[nodiscard]] bool TryParseRuntimeCatalogDependencyKind(
        std::string_view value,
        RuntimeCatalogDependencyKind& outKind) noexcept;
    [[nodiscard]] bool TryParseRuntimeCatalogDependencySource(
        std::string_view value,
        RuntimeCatalogDependencySource& outSource) noexcept;

    struct RuntimeCatalogDependency
    {
        std::string Path;
        std::string CacheKey;
        RuntimeCatalogDependencyKind Kind = RuntimeCatalogDependencyKind::PackagingOnly;
        RuntimeCatalogDependencySource Source = RuntimeCatalogDependencySource::Heuristic;
    };

    struct RuntimeResourceCatalogEntry
    {
        std::string Path;
        std::string CacheKey;
        RuntimeCatalogResourceType Type = RuntimeCatalogResourceType::Data;
        uint32_t CookFormatVersion = 0;
        std::vector<RuntimeCatalogDependency> Dependencies;
        std::vector<std::string> Tags;
        uint64_t CpuBytes = 0;
        uint64_t GpuBytes = 0;
    };

    class RuntimeResourceCatalogSnapshot final
    {
    public:
        [[nodiscard]] static std::shared_ptr<const RuntimeResourceCatalogSnapshot> Empty();
        [[nodiscard]] static std::shared_ptr<const RuntimeResourceCatalogSnapshot> Create(
            std::vector<RuntimeResourceCatalogEntry> entries,
            std::string registryHash);

        [[nodiscard]] static std::string ComputeRegistryHash(
            const std::vector<RuntimeResourceCatalogEntry>& entries);

        [[nodiscard]] const RuntimeResourceCatalogEntry* Find(std::string_view projectPath) const;
        [[nodiscard]] std::vector<const RuntimeResourceCatalogEntry*> FindByTag(
            std::string_view tag) const;
        [[nodiscard]] std::vector<const RuntimeResourceCatalogEntry*> Expand(
            std::string_view rootProjectPath,
            RuntimeCatalogExpand expand) const;

        [[nodiscard]] const std::vector<RuntimeResourceCatalogEntry>& GetEntries() const noexcept;
        [[nodiscard]] const std::string& GetRegistryHash() const noexcept;
        [[nodiscard]] bool IsEmpty() const noexcept;

    private:
        RuntimeResourceCatalogSnapshot() = default;
        RuntimeResourceCatalogSnapshot(
            std::vector<RuntimeResourceCatalogEntry> entries,
            std::string registryHash);

        std::vector<RuntimeResourceCatalogEntry> mEntries;
        std::unordered_map<std::string, size_t> mPathIndices;
        std::unordered_map<std::string, std::vector<size_t>> mTagIndices;
        std::string mRegistryHash;
    };

    class RuntimeResourceCatalog final
    {
    public:
        [[nodiscard]] static RuntimeResourceCatalog& GetInstance();

        void Install(std::shared_ptr<const RuntimeResourceCatalogSnapshot> snapshot) noexcept;
        void Clear() noexcept;
        [[nodiscard]] std::shared_ptr<const RuntimeResourceCatalogSnapshot> GetSnapshot() const noexcept;

    private:
        RuntimeResourceCatalog();

        // C++20 libc++ used by Emscripten lacks atomic<shared_ptr>; the standard
        // shared_ptr atomic free functions preserve the same publication contract.
        std::shared_ptr<const RuntimeResourceCatalogSnapshot> mSnapshot;
    };
}
