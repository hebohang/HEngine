#pragma once

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace HEngine
{
    struct BindingLayoutKey
    {
        uint64_t Value = 0;

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return Value != 0; }
        [[nodiscard]] bool operator==(const BindingLayoutKey&) const = default;
    };

    struct BindlessLayoutKey
    {
        uint64_t Value = 0;

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return Value != 0; }
        [[nodiscard]] bool operator==(const BindlessLayoutKey&) const = default;
    };

    struct StaticBindingSetKey
    {
        uint64_t Value = 0;

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return Value != 0; }
        [[nodiscard]] bool operator==(const StaticBindingSetKey&) const = default;
    };

    struct GraphicsPipelineKey
    {
        uint64_t Value = 0;

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return Value != 0; }
        [[nodiscard]] bool operator==(const GraphicsPipelineKey&) const = default;
    };

    struct ComputePipelineKey
    {
        uint64_t Value = 0;

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return Value != 0; }
        [[nodiscard]] bool operator==(const ComputePipelineKey&) const = default;
    };

    // The caller supplies stable, content-derived keys. NVRHI objects remain the
    // ownership boundary; this class only prevents rebuilding immutable state.
    class PipelineCache final
    {
    public:
        explicit PipelineCache(nvrhi::IDevice* device);
        ~PipelineCache() = default;

        PipelineCache(const PipelineCache&) = delete;
        PipelineCache& operator=(const PipelineCache&) = delete;

        [[nodiscard]] nvrhi::BindingLayoutHandle GetOrCreateBindingLayout(
            BindingLayoutKey key,
            const nvrhi::BindingLayoutDesc& desc,
            std::string_view debugIdentity = {});
        [[nodiscard]] nvrhi::BindingLayoutHandle GetOrCreateBindlessLayout(
            BindlessLayoutKey key,
            const nvrhi::BindlessLayoutDesc& desc,
            std::string_view debugIdentity = {});
        [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateStaticBindingSet(
            StaticBindingSetKey key,
            const nvrhi::BindingSetDesc& desc,
            const nvrhi::BindingLayoutHandle& layout,
            std::string_view debugIdentity = {});
        [[nodiscard]] nvrhi::GraphicsPipelineHandle GetOrCreateGraphicsPipeline(
            GraphicsPipelineKey key,
            const nvrhi::GraphicsPipelineDesc& desc,
            const nvrhi::FramebufferInfo& framebufferInfo,
            std::string_view debugIdentity = {});
        [[nodiscard]] nvrhi::ComputePipelineHandle GetOrCreateComputePipeline(
            ComputePipelineKey key,
            const nvrhi::ComputePipelineDesc& desc,
            std::string_view debugIdentity = {});

        void Clear();

        [[nodiscard]] size_t GetBindingLayoutCount() const;
        [[nodiscard]] size_t GetBindlessLayoutCount() const;
        [[nodiscard]] size_t GetStaticBindingSetCount() const;
        [[nodiscard]] size_t GetGraphicsPipelineCount() const;
        [[nodiscard]] size_t GetComputePipelineCount() const;

    private:
        template<typename Handle>
        struct CacheEntry
        {
            Handle Resource;
            uint64_t DescriptorFingerprint = 0;
            std::string DebugIdentity;
        };

        template<typename Key>
        struct KeyHash
        {
            [[nodiscard]] size_t operator()(Key key) const noexcept
            {
                return std::hash<uint64_t>{}(key.Value);
            }
        };

        nvrhi::DeviceHandle mDevice;
        mutable std::mutex mMutex;
        std::unordered_map<BindingLayoutKey, CacheEntry<nvrhi::BindingLayoutHandle>, KeyHash<BindingLayoutKey>> mBindingLayouts;
        std::unordered_map<BindlessLayoutKey, CacheEntry<nvrhi::BindingLayoutHandle>, KeyHash<BindlessLayoutKey>> mBindlessLayouts;
        std::unordered_map<StaticBindingSetKey, CacheEntry<nvrhi::BindingSetHandle>, KeyHash<StaticBindingSetKey>> mStaticBindingSets;
        std::unordered_map<GraphicsPipelineKey, CacheEntry<nvrhi::GraphicsPipelineHandle>, KeyHash<GraphicsPipelineKey>> mGraphicsPipelines;
        std::unordered_map<ComputePipelineKey, CacheEntry<nvrhi::ComputePipelineHandle>, KeyHash<ComputePipelineKey>> mComputePipelines;
    };
}
