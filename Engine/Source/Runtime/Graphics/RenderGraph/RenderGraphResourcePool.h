#pragma once

#include "Runtime/Graphics/RenderGraph/RenderGraph.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace HEngine
{
    // Uses the same capability normalization as pool compatibility checks.
    [[nodiscard]] nvrhi::BufferDesc MakeRenderGraphBufferAllocationDesc(
        const RenderGraphBufferDesc& desc, RenderGraphUsage requiredUsage,
        uint32_t frameSlot = 0, bool bTransient = false);

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

        [[nodiscard]] constexpr bool IsUnsubmitted() const noexcept
        {
            return !bRecording && LastSubmission == 0;
        }

        [[nodiscard]] constexpr bool CanBegin(uint64_t completedSubmission) const noexcept
        {
            return !bRecording && LastSubmission <= completedSubmission;
        }
    };

    [[nodiscard]] constexpr bool CanTrimRenderGraphTransient(
        RenderGraphFrameSlotState slot, size_t allocationIndex, size_t usedCount,
        uint64_t allocationSubmission, uint64_t completedSubmission) noexcept
    {
        return slot.CanBegin(completedSubmission) && allocationIndex >= usedCount &&
            allocationSubmission <= completedSubmission;
    }

    // Captures the logical interval assigned to one physical allocation. The
    // allocator may reuse the same texture object across disjoint lifetimes;
    // this is not placed-resource heap aliasing.
    struct RenderGraphPoolAllocationInterval
    {
        RenderGraphPoolResourceKey Resource;
        RenderGraphResourceLifetime Lifetime = RenderGraphResourceLifetime::Transient;
        RenderGraphResourceLifetimeInfo PassInterval;
        uint64_t PhysicalAllocationId = 0;
        bool bTextureReuseEligible = false;
    };

    [[nodiscard]] inline bool CanReuseRenderGraphTextureAllocation(
        std::span<const RenderGraphPoolAllocationInterval> intervals,
        uint64_t physicalId, const RenderGraphPoolResourceKey& key,
        RenderGraphResourceLifetimeInfo interval) noexcept
    {
        if (key.Type != RenderGraphPoolResourceType::Texture || !interval.IsUsed() ||
            interval.LastUse == InvalidRenderGraphIndex || interval.FirstUse > interval.LastUse)
            return false;
        bool bFound = false;
        for (const auto& previous : intervals)
        {
            if (previous.PhysicalAllocationId != physicalId) continue;
            bFound = true;
            if (!previous.bTextureReuseEligible || previous.Resource.GraphNamespace != key.GraphNamespace ||
                previous.Resource.Type != RenderGraphPoolResourceType::Texture ||
                previous.Lifetime != RenderGraphResourceLifetime::Transient || !previous.PassInterval.IsUsed() ||
                previous.PassInterval.LastUse == InvalidRenderGraphIndex ||
                previous.PassInterval.FirstUse > previous.PassInterval.LastUse ||
                !(previous.PassInterval.LastUse < interval.FirstUse || interval.LastUse < previous.PassInterval.FirstUse))
                return false;
        }
        return bFound;
    }

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
        uint64_t CachedTransientBytes = 0;
        uint64_t RetiredResourceBytes = 0;
        uint64_t TransientCacheBudgetBytes = 0;
        uint64_t TrimmedTransientBytes = 0;
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
        explicit RenderGraphResourcePool(nvrhi::IDevice* device, uint32_t frameSlotCount,
            bool bTransientTextureReuse = false);
        ~RenderGraphResourcePool();

        RenderGraphResourcePool(const RenderGraphResourcePool&) = delete;
        RenderGraphResourcePool& operator=(const RenderGraphResourcePool&) = delete;

        void BeginFrame(uint32_t frameSlot, uint64_t completedSubmission);
        void EndFrame(uint32_t frameSlot, uint64_t submission);
        // Finish CPU-only transaction preparation without inventing a GPU
        // submission. Keep its realized working set protected from cache trim
        // until this slot begins its first real frame.
        void EndPreparation(uint32_t frameSlot);
        void AbortFrame(uint32_t frameSlot) noexcept;
        void Collect(uint64_t completedSubmission);
        // Budget applies only to unused completed transient allocations. Active
        // working sets and in-flight allocations are protected even above budget.
        void SetTransientCacheBudget(uint64_t bytes);
        // Only exchange pools for the same device/frame-slot count, with no
        // active recording scope. The caller retires the old pool by its fence.
        void Swap(RenderGraphResourcePool& other) noexcept;
        // Transaction preparation only: share completed transient allocations
        // without changing the source. The caller must execute at most one of
        // the pools, committing or discarding the candidate before recording.
        // Persistent/imported resources are never shared by this operation.
        void SeedCompletedTransients(const RenderGraphResourcePool& source,
            uint64_t completedSubmission);
        [[nodiscard]] uint64_t GetLastSubmission() const noexcept;

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
            RenderGraphResourceLifetimeInfo passInterval,
            bool bTextureReuseEligible);
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
