#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace HEngine
{
    enum class RuntimeResourceKind : uint8_t
    {
        Generic,
        Mesh,
        Texture,
        Vfx,
        Prefab,
        Ui,
        Hdr,
        Scene
    };

    enum class RuntimeResourceState : uint8_t
    {
        Unloaded,
        Queued,
        LoadingCpu,
        PendingMainThread,
        Ready,
        Failed,
        EvictPending
    };

    struct RuntimeResourceKey
    {
        RuntimeResourceKind Kind = RuntimeResourceKind::Generic;
        std::string Path;
        std::string CachePath;
        std::string Variant;
        bool bBuiltin = false;

        [[nodiscard]] static RuntimeResourceKey FromProjectPath(
            RuntimeResourceKind kind,
            std::string_view path,
            std::string_view variant = {});
        [[nodiscard]] static RuntimeResourceKey Builtin(
            RuntimeResourceKind kind,
            std::string_view name,
            std::string_view variant = {});

        [[nodiscard]] bool operator==(const RuntimeResourceKey& other) const noexcept;
    };

    struct RuntimeResourceKeyHash
    {
        [[nodiscard]] size_t operator()(const RuntimeResourceKey& key) const noexcept;
    };

    struct RuntimeResourceStats
    {
        uint64_t PhysicalLoads = 0;
        uint64_t CacheHits = 0;
        uint64_t InflightJoins = 0;
        uint64_t Failures = 0;
        uint64_t Resident = 0;
        uint64_t ResidentBytes = 0;
        uint64_t Evictions = 0;
    };

    struct RuntimeResourceGroupProgress
    {
        size_t Total = 0;
        size_t Ready = 0;
        size_t Failed = 0;
        size_t Loading = 0;
        size_t Unloaded = 0;
        float Ratio = 1.0f;
    };
}
