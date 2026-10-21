#include "Hepch.h"

#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceCatalog.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace HEngine
{
    namespace
    {
        constexpr uint64_t FnvOffsetBasis = 14695981039346656037ull;
        constexpr uint64_t FnvPrime = 1099511628211ull;

        bool IsValid(RuntimeCatalogResourceType type) noexcept
        {
            return static_cast<uint8_t>(type) <=
                static_cast<uint8_t>(RuntimeCatalogResourceType::Collision);
        }

        bool IsValid(RuntimeCatalogDependencyKind kind) noexcept
        {
            return static_cast<uint8_t>(kind) <=
                static_cast<uint8_t>(RuntimeCatalogDependencyKind::PackagingOnly);
        }

        bool IsValid(RuntimeCatalogDependencySource source) noexcept
        {
            return static_cast<uint8_t>(source) <=
                static_cast<uint8_t>(RuntimeCatalogDependencySource::Heuristic);
        }

        void HashBytes(uint64_t& hash, const void* data, size_t size) noexcept
        {
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t index = 0; index < size; ++index)
            {
                hash ^= bytes[index];
                hash *= FnvPrime;
            }
        }

        void HashUint64LittleEndian(uint64_t& hash, uint64_t value) noexcept
        {
            uint8_t bytes[sizeof(value)]{};
            for (size_t index = 0; index < sizeof(value); ++index)
                bytes[index] = static_cast<uint8_t>((value >> (index * 8)) & 0xffu);
            HashBytes(hash, bytes, sizeof(bytes));
        }

        void HashLengthPrefixedString(uint64_t& hash, std::string_view value) noexcept
        {
            HashUint64LittleEndian(hash, static_cast<uint64_t>(value.size()));
            HashBytes(hash, value.data(), value.size());
        }

        std::string Hex64(uint64_t value)
        {
            std::ostringstream stream;
            stream << std::hex << std::setfill('0') << std::setw(16) << std::nouppercase << value;
            return stream.str();
        }

        bool IsStrictlyNormalizedProjectPath(std::string_view path, std::string& outCacheKey)
        {
            if (path.empty() || path.size() > RuntimeResourceCatalogLimits::MaxPathLength)
                return false;

            std::string normalized;
            if (!Utils::TryNormalizeProjectRelativePath(path, normalized) || normalized != path)
                return false;
            outCacheKey = Utils::MakeProjectPathCacheKey(normalized);
            return true;
        }

        bool IsValidRegistryHash(std::string_view hash) noexcept
        {
            return hash.size() == 16 && std::all_of(hash.begin(), hash.end(), [](char character)
            {
                return (character >= '0' && character <= '9') ||
                    (character >= 'a' && character <= 'f');
            });
        }

        bool IsValidTag(std::string_view tag) noexcept
        {
            return !tag.empty() && tag.size() <= RuntimeResourceCatalogLimits::MaxTagLength &&
                std::none_of(tag.begin(), tag.end(), [](char character)
                {
                    return static_cast<unsigned char>(character) < 32;
                });
        }

        bool IsBytewiseLess(std::string_view lhs, std::string_view rhs) noexcept
        {
            return std::lexicographical_compare(
                lhs.begin(),
                lhs.end(),
                rhs.begin(),
                rhs.end(),
                [](char lhsCharacter, char rhsCharacter)
                {
                    return static_cast<unsigned char>(lhsCharacter) <
                        static_cast<unsigned char>(rhsCharacter);
                });
        }
    }

    const char* ToString(RuntimeCatalogResourceType type) noexcept
    {
        switch (type)
        {
        case RuntimeCatalogResourceType::Scene: return "Scene";
        case RuntimeCatalogResourceType::Prefab: return "Prefab";
        case RuntimeCatalogResourceType::Mesh: return "Mesh";
        case RuntimeCatalogResourceType::Material: return "Material";
        case RuntimeCatalogResourceType::Skeleton: return "Skeleton";
        case RuntimeCatalogResourceType::AnimationClip: return "AnimationClip";
        case RuntimeCatalogResourceType::AnimationController: return "AnimationController";
        case RuntimeCatalogResourceType::UiDocument: return "UiDocument";
        case RuntimeCatalogResourceType::BehaviorTree: return "BehaviorTree";
        case RuntimeCatalogResourceType::UiMarkup: return "UiMarkup";
        case RuntimeCatalogResourceType::Texture: return "Texture";
        case RuntimeCatalogResourceType::ShaderSource: return "ShaderSource";
        case RuntimeCatalogResourceType::ShaderBinary: return "ShaderBinary";
        case RuntimeCatalogResourceType::Script: return "Script";
        case RuntimeCatalogResourceType::Audio: return "Audio";
        case RuntimeCatalogResourceType::Font: return "Font";
        case RuntimeCatalogResourceType::EffectPackage: return "EffectPackage";
        case RuntimeCatalogResourceType::Effect: return "Effect";
        case RuntimeCatalogResourceType::MeshSource: return "MeshSource";
        case RuntimeCatalogResourceType::Config: return "Config";
        case RuntimeCatalogResourceType::Binary: return "Binary";
        case RuntimeCatalogResourceType::Data: return "Data";
        case RuntimeCatalogResourceType::Collision: return "Collision";
        }
        return "Data";
    }

    const char* ToString(RuntimeCatalogDependencyKind kind) noexcept
    {
        switch (kind)
        {
        case RuntimeCatalogDependencyKind::HardRuntime: return "HardRuntime";
        case RuntimeCatalogDependencyKind::SoftRuntime: return "SoftRuntime";
        case RuntimeCatalogDependencyKind::PackagingOnly: return "PackagingOnly";
        }
        return "PackagingOnly";
    }

    const char* ToString(RuntimeCatalogDependencySource source) noexcept
    {
        switch (source)
        {
        case RuntimeCatalogDependencySource::Structured: return "Structured";
        case RuntimeCatalogDependencySource::Heuristic: return "Heuristic";
        }
        return "Heuristic";
    }

    bool TryParseRuntimeCatalogResourceType(
        std::string_view value,
        RuntimeCatalogResourceType& outType) noexcept
    {
#define HE_TRY_PARSE_CATALOG_TYPE(Name) \
        if (value == #Name) { outType = RuntimeCatalogResourceType::Name; return true; }
        HE_TRY_PARSE_CATALOG_TYPE(Scene)
        HE_TRY_PARSE_CATALOG_TYPE(Prefab)
        HE_TRY_PARSE_CATALOG_TYPE(Mesh)
        HE_TRY_PARSE_CATALOG_TYPE(Material)
        HE_TRY_PARSE_CATALOG_TYPE(Skeleton)
        HE_TRY_PARSE_CATALOG_TYPE(AnimationClip)
        HE_TRY_PARSE_CATALOG_TYPE(AnimationController)
        HE_TRY_PARSE_CATALOG_TYPE(UiDocument)
        HE_TRY_PARSE_CATALOG_TYPE(BehaviorTree)
        HE_TRY_PARSE_CATALOG_TYPE(UiMarkup)
        HE_TRY_PARSE_CATALOG_TYPE(Texture)
        HE_TRY_PARSE_CATALOG_TYPE(ShaderSource)
        HE_TRY_PARSE_CATALOG_TYPE(ShaderBinary)
        HE_TRY_PARSE_CATALOG_TYPE(Script)
        HE_TRY_PARSE_CATALOG_TYPE(Audio)
        HE_TRY_PARSE_CATALOG_TYPE(Font)
        HE_TRY_PARSE_CATALOG_TYPE(EffectPackage)
        HE_TRY_PARSE_CATALOG_TYPE(Effect)
        HE_TRY_PARSE_CATALOG_TYPE(MeshSource)
        HE_TRY_PARSE_CATALOG_TYPE(Config)
        HE_TRY_PARSE_CATALOG_TYPE(Binary)
        HE_TRY_PARSE_CATALOG_TYPE(Data)
        HE_TRY_PARSE_CATALOG_TYPE(Collision)
#undef HE_TRY_PARSE_CATALOG_TYPE
        return false;
    }

    bool TryParseRuntimeCatalogDependencyKind(
        std::string_view value,
        RuntimeCatalogDependencyKind& outKind) noexcept
    {
        if (value == "HardRuntime")
            outKind = RuntimeCatalogDependencyKind::HardRuntime;
        else if (value == "SoftRuntime")
            outKind = RuntimeCatalogDependencyKind::SoftRuntime;
        else if (value == "PackagingOnly")
            outKind = RuntimeCatalogDependencyKind::PackagingOnly;
        else
            return false;
        return true;
    }

    bool TryParseRuntimeCatalogDependencySource(
        std::string_view value,
        RuntimeCatalogDependencySource& outSource) noexcept
    {
        if (value == "Structured")
            outSource = RuntimeCatalogDependencySource::Structured;
        else if (value == "Heuristic")
            outSource = RuntimeCatalogDependencySource::Heuristic;
        else
            return false;
        return true;
    }

    std::shared_ptr<const RuntimeResourceCatalogSnapshot> RuntimeResourceCatalogSnapshot::Empty()
    {
        static const std::shared_ptr<const RuntimeResourceCatalogSnapshot> empty(
            new RuntimeResourceCatalogSnapshot());
        return empty;
    }

    std::shared_ptr<const RuntimeResourceCatalogSnapshot> RuntimeResourceCatalogSnapshot::Create(
        std::vector<RuntimeResourceCatalogEntry> entries,
        std::string registryHash)
    {
        if (entries.empty())
            throw std::invalid_argument("A versioned runtime resource catalog cannot be empty.");
        if (entries.size() > RuntimeResourceCatalogLimits::MaxEntries)
            throw std::invalid_argument("The runtime resource catalog exceeds its entry limit.");
        for (const RuntimeResourceCatalogEntry& entry : entries)
        {
            if (!IsValid(entry.Type))
                throw std::invalid_argument("The runtime resource catalog contains an invalid resource type.");
            for (const RuntimeCatalogDependency& dependency : entry.Dependencies)
            {
                if (!IsValid(dependency.Kind) || !IsValid(dependency.Source))
                    throw std::invalid_argument("The runtime resource catalog contains an invalid dependency enum.");
            }
        }
        if (!IsValidRegistryHash(registryHash))
            throw std::invalid_argument("The runtime resource catalog hash is invalid.");
        if (ComputeRegistryHash(entries) != registryHash)
            throw std::invalid_argument("The runtime resource catalog hash does not match its entries.");

        return std::shared_ptr<const RuntimeResourceCatalogSnapshot>(
            new RuntimeResourceCatalogSnapshot(std::move(entries), std::move(registryHash)));
    }

    std::string RuntimeResourceCatalogSnapshot::ComputeRegistryHash(
        const std::vector<RuntimeResourceCatalogEntry>& entries)
    {
        uint64_t hash = FnvOffsetBasis;
        HashLengthPrefixedString(hash, RuntimeResourceCatalogLimits::RegistryHashDomain);
        HashUint64LittleEndian(hash, static_cast<uint64_t>(entries.size()));
        for (const RuntimeResourceCatalogEntry& entry : entries)
        {
            HashLengthPrefixedString(hash, entry.Path);
            HashLengthPrefixedString(hash, ToString(entry.Type));
            HashUint64LittleEndian(hash, entry.CookFormatVersion);
            HashUint64LittleEndian(hash, static_cast<uint64_t>(entry.Dependencies.size()));
            for (const RuntimeCatalogDependency& dependency : entry.Dependencies)
            {
                HashLengthPrefixedString(hash, dependency.Path);
                HashLengthPrefixedString(hash, ToString(dependency.Kind));
                HashLengthPrefixedString(hash, ToString(dependency.Source));
            }
            HashUint64LittleEndian(hash, static_cast<uint64_t>(entry.Tags.size()));
            for (const std::string& tag : entry.Tags)
                HashLengthPrefixedString(hash, tag);
            HashUint64LittleEndian(hash, entry.CpuBytes);
            HashUint64LittleEndian(hash, entry.GpuBytes);
        }
        return Hex64(hash);
    }

    RuntimeResourceCatalogSnapshot::RuntimeResourceCatalogSnapshot(
        std::vector<RuntimeResourceCatalogEntry> entries,
        std::string registryHash)
        : mEntries(std::move(entries)), mRegistryHash(std::move(registryHash))
    {
        std::uint64_t totalDependencies = 0;
        std::uint64_t totalTags = 0;
        std::uint64_t totalCpuBytes = 0;
        std::uint64_t totalGpuBytes = 0;
        mPathIndices.reserve(mEntries.size());
        for (size_t entryIndex = 0; entryIndex < mEntries.size(); ++entryIndex)
        {
            RuntimeResourceCatalogEntry& entry = mEntries[entryIndex];
            if (!IsStrictlyNormalizedProjectPath(entry.Path, entry.CacheKey))
                throw std::invalid_argument("The runtime resource catalog contains an invalid path: " + entry.Path);
            if (!mPathIndices.emplace(entry.CacheKey, entryIndex).second)
                throw std::invalid_argument("The runtime resource catalog contains a duplicate path: " + entry.Path);
            if (entry.Dependencies.size() > RuntimeResourceCatalogLimits::MaxDependenciesPerEntry ||
                totalDependencies > RuntimeResourceCatalogLimits::MaxDependencies - entry.Dependencies.size())
            {
                throw std::invalid_argument("A runtime resource catalog entry exceeds its dependency limit: " + entry.Path);
            }
            totalDependencies += static_cast<std::uint64_t>(entry.Dependencies.size());
            if (entry.Tags.size() > RuntimeResourceCatalogLimits::MaxTagsPerEntry ||
                totalTags > RuntimeResourceCatalogLimits::MaxTags - entry.Tags.size())
            {
                throw std::invalid_argument("A runtime resource catalog entry exceeds its tag limit: " + entry.Path);
            }
            totalTags += static_cast<std::uint64_t>(entry.Tags.size());
            if (entry.CpuBytes > RuntimeResourceCatalogLimits::MaxResourceEstimate ||
                entry.GpuBytes > RuntimeResourceCatalogLimits::MaxResourceEstimate ||
                totalCpuBytes > RuntimeResourceCatalogLimits::MaxCatalogEstimate - entry.CpuBytes ||
                totalGpuBytes > RuntimeResourceCatalogLimits::MaxCatalogEstimate - entry.GpuBytes)
            {
                throw std::invalid_argument("The runtime resource catalog exceeds its resource estimate limit.");
            }
            totalCpuBytes += entry.CpuBytes;
            totalGpuBytes += entry.GpuBytes;

            std::unordered_set<std::string> dependencyKeys;
            for (RuntimeCatalogDependency& dependency : entry.Dependencies)
            {
                if (!IsStrictlyNormalizedProjectPath(dependency.Path, dependency.CacheKey))
                    throw std::invalid_argument("The runtime resource catalog contains an invalid dependency path.");
                if (!dependencyKeys.emplace(dependency.CacheKey).second)
                    throw std::invalid_argument("The runtime resource catalog contains a duplicate dependency.");
                if (dependency.Source == RuntimeCatalogDependencySource::Heuristic &&
                    dependency.Kind != RuntimeCatalogDependencyKind::PackagingOnly)
                {
                    throw std::invalid_argument("Heuristic catalog dependencies must be packaging-only.");
                }
            }

            std::unordered_set<std::string> tags;
            for (const std::string& tag : entry.Tags)
            {
                if (!IsValidTag(tag) || !tags.emplace(tag).second)
                    throw std::invalid_argument("The runtime resource catalog contains an invalid or duplicate tag.");
                mTagIndices[tag].push_back(entryIndex);
            }
        }

        for (const RuntimeResourceCatalogEntry& entry : mEntries)
        {
            for (const RuntimeCatalogDependency& dependency : entry.Dependencies)
            {
                const auto found = mPathIndices.find(dependency.CacheKey);
                if (found == mPathIndices.end() || mEntries[found->second].Path != dependency.Path)
                    throw std::invalid_argument("The runtime resource catalog contains a non-canonical dependency target.");
            }
        }

        for (auto& [tag, entryIndices] : mTagIndices)
        {
            (void)tag;
            std::sort(entryIndices.begin(), entryIndices.end(), [this](size_t lhs, size_t rhs)
            {
                return IsBytewiseLess(mEntries[lhs].Path, mEntries[rhs].Path);
            });
        }
    }

    const RuntimeResourceCatalogEntry* RuntimeResourceCatalogSnapshot::Find(
        std::string_view projectPath) const
    {
        std::string normalized;
        if (projectPath.size() > RuntimeResourceCatalogLimits::MaxPathLength ||
            !Utils::TryNormalizeProjectRelativePath(projectPath, normalized))
        {
            return nullptr;
        }

        const auto found = mPathIndices.find(Utils::MakeProjectPathCacheKey(normalized));
        return found == mPathIndices.end() ? nullptr : &mEntries[found->second];
    }

    std::vector<const RuntimeResourceCatalogEntry*> RuntimeResourceCatalogSnapshot::FindByTag(
        std::string_view tag) const
    {
        std::vector<const RuntimeResourceCatalogEntry*> result;
        const auto found = mTagIndices.find(std::string(tag));
        if (found == mTagIndices.end())
            return result;

        result.reserve(found->second.size());
        for (size_t entryIndex : found->second)
            result.push_back(&mEntries[entryIndex]);
        return result;
    }

    std::vector<const RuntimeResourceCatalogEntry*> RuntimeResourceCatalogSnapshot::Expand(
        std::string_view rootProjectPath,
        RuntimeCatalogExpand expand) const
    {
        const RuntimeResourceCatalogEntry* root = Find(rootProjectPath);
        if (root == nullptr)
            return {};

        const size_t rootIndex = static_cast<size_t>(root - mEntries.data());
        std::vector<size_t> pending = { rootIndex };
        std::unordered_set<size_t> included = { rootIndex };
        while (!pending.empty())
        {
            const size_t entryIndex = pending.back();
            pending.pop_back();
            for (const RuntimeCatalogDependency& dependency : mEntries[entryIndex].Dependencies)
            {
                if (dependency.Kind != RuntimeCatalogDependencyKind::HardRuntime &&
                    !(expand == RuntimeCatalogExpand::Runtime &&
                        dependency.Kind == RuntimeCatalogDependencyKind::SoftRuntime))
                {
                    continue;
                }

                const size_t dependencyIndex = mPathIndices.at(dependency.CacheKey);
                if (included.emplace(dependencyIndex).second)
                    pending.push_back(dependencyIndex);
            }
        }

        std::vector<const RuntimeResourceCatalogEntry*> result;
        result.reserve(included.size());
        for (size_t entryIndex = 0; entryIndex < mEntries.size(); ++entryIndex)
        {
            if (included.contains(entryIndex))
                result.push_back(&mEntries[entryIndex]);
        }
        std::sort(result.begin(), result.end(), [](const auto* lhs, const auto* rhs)
        {
            return IsBytewiseLess(lhs->Path, rhs->Path);
        });
        return result;
    }

    const std::vector<RuntimeResourceCatalogEntry>& RuntimeResourceCatalogSnapshot::GetEntries() const noexcept
    {
        return mEntries;
    }

    const std::string& RuntimeResourceCatalogSnapshot::GetRegistryHash() const noexcept
    {
        return mRegistryHash;
    }

    bool RuntimeResourceCatalogSnapshot::IsEmpty() const noexcept
    {
        return mEntries.empty();
    }

    RuntimeResourceCatalog& RuntimeResourceCatalog::GetInstance()
    {
        static RuntimeResourceCatalog instance;
        return instance;
    }

    RuntimeResourceCatalog::RuntimeResourceCatalog()
        : mSnapshot(RuntimeResourceCatalogSnapshot::Empty())
    {
    }

    void RuntimeResourceCatalog::Install(
        std::shared_ptr<const RuntimeResourceCatalogSnapshot> snapshot) noexcept
    {
        if (!snapshot)
            snapshot = RuntimeResourceCatalogSnapshot::Empty();
        std::atomic_store_explicit(&mSnapshot, std::move(snapshot), std::memory_order_release);
    }

    void RuntimeResourceCatalog::Clear() noexcept
    {
        Install(RuntimeResourceCatalogSnapshot::Empty());
    }

    std::shared_ptr<const RuntimeResourceCatalogSnapshot> RuntimeResourceCatalog::GetSnapshot() const noexcept
    {
        return std::atomic_load_explicit(&mSnapshot, std::memory_order_acquire);
    }
}
