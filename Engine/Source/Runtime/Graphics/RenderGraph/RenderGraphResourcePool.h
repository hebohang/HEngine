#pragma once

#include "Runtime/Graphics/RenderGraph/RenderGraph.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace HEngine
{
    enum class RenderGraphPoolResourceType : uint8_t
    {
        Texture,
        Buffer,
    };

    struct RenderGraphPoolResourceKey
    {
        uint64_t GraphNamespace = 0;
        uint32_t ResourceIndex = InvalidRenderGraphIndex;
        uint32_t ResourceGeneration = 0;
        RenderGraphPoolResourceType Type = RenderGraphPoolResourceType::Texture;

        [[nodiscard]] bool operator==(const RenderGraphPoolResourceKey&) const = default;
    };

    struct RenderGraphFrameSlotState
    {
        uint64_t LastSubmission = 0;
        bool bRecording = false;

        [[nodiscard]] constexpr bool CanBegin(uint64_t completedSubmission) const noexcept
        {
            return !bRecording && LastSubmission <= completedSubmission;
        }
    };

    // Captures the logical interval assigned to one physical allocation. The
    // first implementation intentionally does not alias memory; these records
    // are the input for a later placed-resource alias planner.
    struct RenderGraphPoolAllocationInterval
    {
        RenderGraphPoolResourceKey Resource;
        RenderGraphResourceLifetime Lifetime = RenderGraphResourceLifetime::Transient;
        RenderGraphResourceLifetimeInfo PassInterval;
        uint64_t PhysicalAllocationId = 0;
    };

    struct RenderGraphResourcePoolStats
    {
        size_t PersistentTextureCount = 0;
        size_t PersistentBufferCount = 0;
        size_t TransientTextureCount = 0;
        size_t TransientBufferCount = 0;
        size_t RetiredResourceCount = 0;
        uint64_t PersistentTextureBytes = 0;
        uint64_t PersistentBufferBytes = 0;
        uint64_t TransientTextureBytes = 0;
        uint64_t TransientBufferBytes = 0;
        uint64_t PeakFrameTransientBytes = 0;
    };

    // RenderGraphResourcePool is owned beside the RenderDevice; it is neither a
    // global nor a singleton. BeginFrame/EndFrame bracket command recording for
    // one frame slot. A slot cannot be reused until its previous submission is
    // known complete, which prevents transient allocations from being recycled
    // while the GPU still references them. Persistent resources may be shared
    // by frames submitted to the same ordered graphics queue; their graph-boundary
    // state is Common, so queue order also defines their cross-frame dependency.
    class RenderGraphResourcePool final
    {
    public:
        explicit RenderGraphResourcePool(nvrhi::IDevice* device, uint32_t frameSlotCount);
        ~RenderGraphResourcePool();

        RenderGraphResourcePool(const RenderGraphResourcePool&) = delete;
        RenderGraphResourcePool& operator=(const RenderGraphResourcePool&) = delete;

        void BeginFrame(uint32_t frameSlot, uint64_t completedSubmission);
        void EndFrame(uint32_t frameSlot, uint64_t submission);
        void AbortFrame(uint32_t frameSlot) noexcept;
        void Collect(uint64_t completedSubmission);

        [[nodiscard]] uint32_t GetFrameSlotCount() const noexcept;
        [[nodiscard]] nvrhi::IDevice* GetDevice() const noexcept;
        [[nodiscard]] bool IsFrameActive(uint32_t frameSlot) const noexcept;
        [[nodiscard]] RenderGraphFrameSlotState GetFrameSlotState(uint32_t frameSlot) const;
        [[nodiscard]] const std::vector<RenderGraphPoolAllocationInterval>&
            GetAllocationIntervals(uint32_t frameSlot) const;
        [[nodiscard]] RenderGraphResourcePoolStats GetStats() const noexcept;

    private:
        friend class RenderGraph;

        [[nodiscard]] nvrhi::TextureHandle AcquireTexture(
            uint32_t frameSlot,
            const RenderGraphPoolResourceKey& key,
            const RenderGraphTextureDesc& desc,
            RenderGraphResourceLifetime lifetime,
            RenderGraphUsage requiredUsage,
            RenderGraphResourceLifetimeInfo passInterval);
        [[nodiscard]] nvrhi::BufferHandle AcquireBuffer(
            uint32_t frameSlot,
            const RenderGraphPoolResourceKey& key,
            const RenderGraphBufferDesc& desc,
            RenderGraphResourceLifetime lifetime,
            RenderGraphUsage requiredUsage,
            RenderGraphResourceLifetimeInfo passInterval);
        void ReleasePersistentResources(uint64_t graphNamespace);

        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
