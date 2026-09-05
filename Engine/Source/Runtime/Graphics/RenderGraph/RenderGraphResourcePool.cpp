#include "Hepch.h"

#include "Runtime/Graphics/RenderGraph/RenderGraphResourcePool.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace HEngine
{
    namespace
    {
        constexpr RenderGraphUsage TextureCapabilityUsages =
            RenderGraphUsage::RenderTarget |
            RenderGraphUsage::DepthRead |
            RenderGraphUsage::DepthWrite |
            RenderGraphUsage::ShaderResource |
            RenderGraphUsage::UnorderedAccess;
        constexpr RenderGraphUsage BufferCapabilityUsages =
            RenderGraphUsage::ShaderResource |
            RenderGraphUsage::UnorderedAccess |
            RenderGraphUsage::AccelerationStructureBuildInput;

        [[nodiscard]] uint32_t UsageBits(RenderGraphUsage usage) noexcept
        {
            return static_cast<uint32_t>(usage);
        }

        [[nodiscard]] bool IncludesUsage(RenderGraphUsage available, RenderGraphUsage required) noexcept
        {
            return (UsageBits(available) & UsageBits(required)) == UsageBits(required);
        }

        [[nodiscard]] RenderGraphUsage TextureCapabilities(RenderGraphUsage usage) noexcept
        {
            return usage & TextureCapabilityUsages;
        }

        [[nodiscard]] RenderGraphUsage BufferCapabilities(RenderGraphUsage usage) noexcept
        {
            return usage & BufferCapabilityUsages;
        }

        [[nodiscard]] bool HasAnyUsage(RenderGraphUsage value, RenderGraphUsage mask) noexcept
        {
            return (value & mask) != RenderGraphUsage::None;
        }

        [[nodiscard]] uint64_t EstimateTextureBytes(
            const RenderGraphTextureDesc& desc) noexcept
        {
            if (desc.Format == nvrhi::Format::UNKNOWN)
                return 0;
            const nvrhi::FormatInfo& format = nvrhi::getFormatInfo(desc.Format);
            const uint64_t blockSize = std::max<uint64_t>(format.blockSize, 1u);
            uint64_t total = 0;
            uint64_t width = std::max<uint64_t>(desc.Width, 1u);
            uint64_t height = std::max<uint64_t>(desc.Height, 1u);
            uint64_t depth = std::max<uint64_t>(desc.Depth, 1u);
            for (uint32_t mip = 0; mip < std::max(desc.MipLevels, 1u); ++mip)
            {
                const uint64_t blocksX = (width + blockSize - 1u) / blockSize;
                const uint64_t blocksY = (height + blockSize - 1u) / blockSize;
                total += blocksX * blocksY * depth * format.bytesPerBlock;
                width = std::max(width >> 1u, 1ull);
                height = std::max(height >> 1u, 1ull);
                depth = std::max(depth >> 1u, 1ull);
            }
            return total * std::max<uint64_t>(desc.ArrayLayers, 1u) *
                std::max<uint64_t>(desc.SampleCount, 1u);
        }

        [[nodiscard]] nvrhi::TextureDesc MakeTextureDesc(
            const RenderGraphTextureDesc& desc,
            RenderGraphUsage capabilities,
            uint32_t frameSlot,
            bool bTransient)
        {
            if (desc.Format == nvrhi::Format::UNKNOWN)
                throw RenderGraphValidationError("RenderGraph cannot realize a texture with UNKNOWN format: " + desc.DebugName);
            if (desc.SampleCount != 1 && desc.SampleCount != 2 &&
                desc.SampleCount != 4 && desc.SampleCount != 8)
            {
                throw RenderGraphValidationError(
                    "RenderGraph texture sample count must be 1, 2, 4, or 8: " +
                    desc.DebugName);
            }
            if (desc.SampleCount > 1)
            {
                if (desc.MipLevels != 1 || desc.Depth != 1)
                {
                    throw RenderGraphValidationError(
                        "Multisampled RenderGraph textures require one mip and depth slice: " +
                        desc.DebugName);
                }
                if (HasRenderGraphUsage(
                        capabilities,
                        RenderGraphUsage::UnorderedAccess))
                {
                    throw RenderGraphValidationError(
                        "Multisampled RenderGraph textures cannot be unordered-access resources: " +
                        desc.DebugName);
                }
                if (desc.Dimension != nvrhi::TextureDimension::Texture2D &&
                    desc.Dimension != nvrhi::TextureDimension::Texture2DArray &&
                    desc.Dimension != nvrhi::TextureDimension::Texture2DMS &&
                    desc.Dimension != nvrhi::TextureDimension::Texture2DMSArray)
                {
                    throw RenderGraphValidationError(
                        "Multisampled RenderGraph textures must be 2D or 2D-array resources: " +
                        desc.DebugName);
                }
            }

            nvrhi::TextureDesc result;
            result.width = desc.Width;
            result.height = desc.Height;
            result.depth = desc.Depth;
            result.arraySize = desc.ArrayLayers;
            result.mipLevels = desc.MipLevels;
            result.sampleCount = desc.SampleCount;
            result.format = desc.Format;
            result.dimension = desc.SampleCount > 1
                ? desc.ArrayLayers > 1 ||
                        desc.Dimension == nvrhi::TextureDimension::Texture2DArray ||
                        desc.Dimension == nvrhi::TextureDimension::Texture2DMSArray
                    ? nvrhi::TextureDimension::Texture2DMSArray
                    : nvrhi::TextureDimension::Texture2DMS
                : desc.Dimension == nvrhi::TextureDimension::Texture2DMSArray
                    ? nvrhi::TextureDimension::Texture2DArray
                    : desc.Dimension == nvrhi::TextureDimension::Texture2DMS
                        ? nvrhi::TextureDimension::Texture2D
                        : desc.Dimension;
            result.debugName = desc.DebugName.empty() ? "RenderGraphTexture" : desc.DebugName;
            if (bTransient)
                result.debugName += " [frame-slot " + std::to_string(frameSlot) + ']';
            // Some DX12 drivers reject RGBA16_FLOAT MSAA render targets when
            // DENY_SHADER_RESOURCE is present even though the corresponding
            // sample-quality query succeeds. Keep color MSAA resources SRV-
            // capable; this is an allocation capability, not a graph access.
            result.isShaderResource = HasAnyUsage(
                capabilities,
                RenderGraphUsage::ShaderResource |
                    RenderGraphUsage::DepthRead |
                    RenderGraphUsage::UnorderedAccess) ||
                (desc.SampleCount > 1 &&
                    !nvrhi::getFormatInfo(desc.Format).hasDepth);
            result.isRenderTarget = HasAnyUsage(
                capabilities,
                RenderGraphUsage::RenderTarget |
                    RenderGraphUsage::DepthRead |
                    RenderGraphUsage::DepthWrite);
            result.isUAV = HasRenderGraphUsage(capabilities, RenderGraphUsage::UnorderedAccess);
            result.clearValue = desc.ClearValue;
            result.useClearValue = desc.bUseClearValue;
            result.initialState = nvrhi::ResourceStates::Common;
            result.keepInitialState = false;
            return result;
        }

        [[nodiscard]] nvrhi::BufferDesc MakeBufferDesc(
            const RenderGraphBufferDesc& desc,
            RenderGraphUsage capabilities,
            uint32_t frameSlot,
            bool bTransient)
        {
            nvrhi::BufferDesc result;
            result.byteSize = desc.ByteSize;
            result.structStride = desc.StructStride;
            result.debugName = desc.DebugName.empty() ? "RenderGraphBuffer" : desc.DebugName;
            if (bTransient)
                result.debugName += " [frame-slot " + std::to_string(frameSlot) + ']';
            result.canHaveUAVs = HasRenderGraphUsage(capabilities, RenderGraphUsage::UnorderedAccess);
            result.isDrawIndirectArgs = HasRenderGraphUsage(
                capabilities,
                RenderGraphUsage::IndirectArgument);
            result.isAccelStructBuildInput = HasRenderGraphUsage(
                capabilities,
                RenderGraphUsage::AccelerationStructureBuildInput);
            result.canHaveRawViews = desc.StructStride == 0 && HasAnyUsage(
                capabilities,
                RenderGraphUsage::ShaderResource | RenderGraphUsage::UnorderedAccess);
            result.initialState = nvrhi::ResourceStates::Common;
            result.keepInitialState = false;
            return result;
        }

        [[nodiscard]] bool SameTextureShape(
            const RenderGraphTextureDesc& left,
            const RenderGraphTextureDesc& right) noexcept
        {
            return left.Width == right.Width &&
                left.Height == right.Height &&
                left.Depth == right.Depth &&
                left.MipLevels == right.MipLevels &&
                left.ArrayLayers == right.ArrayLayers &&
                left.SampleCount == right.SampleCount &&
                left.Format == right.Format &&
                left.Dimension == right.Dimension &&
                left.bUseClearValue == right.bUseClearValue &&
                (!left.bUseClearValue ||
                    (left.ClearValue.r == right.ClearValue.r &&
                        left.ClearValue.g == right.ClearValue.g &&
                        left.ClearValue.b == right.ClearValue.b &&
                        left.ClearValue.a == right.ClearValue.a));
        }

        [[nodiscard]] bool SameBufferShape(
            const RenderGraphBufferDesc& left,
            const RenderGraphBufferDesc& right) noexcept
        {
            return left.ByteSize == right.ByteSize && left.StructStride == right.StructStride;
        }

        struct ResourceKeyHash
        {
            [[nodiscard]] size_t operator()(const RenderGraphPoolResourceKey& key) const noexcept
            {
                size_t hash = std::hash<uint64_t>{}(key.GraphNamespace);
                hash ^= std::hash<uint32_t>{}(key.ResourceIndex) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                hash ^= std::hash<uint32_t>{}(key.ResourceGeneration) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                hash ^= std::hash<uint8_t>{}(static_cast<uint8_t>(key.Type)) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                return hash;
            }
        };
    }

    struct RenderGraphResourcePool::Impl
    {
        struct TextureAllocation
        {
            RenderGraphTextureDesc Desc;
            RenderGraphUsage Capabilities = RenderGraphUsage::None;
            nvrhi::TextureHandle Resource;
            uint64_t PhysicalAllocationId = 0;
            uint64_t LastSubmission = 0;
        };

        struct BufferAllocation
        {
            RenderGraphBufferDesc Desc;
            RenderGraphUsage Capabilities = RenderGraphUsage::None;
            nvrhi::BufferHandle Resource;
            uint64_t PhysicalAllocationId = 0;
            uint64_t LastSubmission = 0;
        };

        struct FrameSlot
        {
            RenderGraphFrameSlotState State;
            uint64_t CompletedSubmissionAtBegin = 0;
            size_t TextureCursor = 0;
            size_t BufferCursor = 0;
            std::vector<TextureAllocation> Textures;
            std::vector<BufferAllocation> Buffers;
            std::vector<RenderGraphPoolResourceKey> PersistentTexturesUsed;
            std::vector<RenderGraphPoolResourceKey> PersistentBuffersUsed;
            std::vector<RenderGraphPoolAllocationInterval> AllocationIntervals;
        };

        struct RetiredResource
        {
            uint64_t Submission = 0;
            nvrhi::TextureHandle Texture;
            nvrhi::BufferHandle Buffer;
        };

        explicit Impl(nvrhi::IDevice* device, uint32_t frameSlotCount)
            : Device(device), FrameSlots(frameSlotCount)
        {
        }

        [[nodiscard]] FrameSlot& RequireFrameSlot(uint32_t frameSlot)
        {
            if (frameSlot >= FrameSlots.size())
                throw std::out_of_range("RenderGraph frame slot is out of range");
            return FrameSlots[frameSlot];
        }

        [[nodiscard]] const FrameSlot& RequireFrameSlot(uint32_t frameSlot) const
        {
            if (frameSlot >= FrameSlots.size())
                throw std::out_of_range("RenderGraph frame slot is out of range");
            return FrameSlots[frameSlot];
        }

        [[nodiscard]] uint64_t AllocatePhysicalId() noexcept
        {
            const uint64_t id = NextPhysicalAllocationId++;
            if (NextPhysicalAllocationId == 0)
                NextPhysicalAllocationId = 1;
            return id;
        }

        void Retire(nvrhi::TextureHandle resource, uint64_t submission)
        {
            if (!resource || submission <= CompletedSubmission)
                return;
            RetiredResources.push_back({ .Submission = submission, .Texture = std::move(resource) });
        }

        void Retire(nvrhi::BufferHandle resource, uint64_t submission)
        {
            if (!resource || submission <= CompletedSubmission)
                return;
            RetiredResources.push_back({ .Submission = submission, .Buffer = std::move(resource) });
        }

        nvrhi::IDevice* Device = nullptr;
        uint64_t CompletedSubmission = 0;
        uint64_t NextPhysicalAllocationId = 1;
        std::vector<FrameSlot> FrameSlots;
        std::unordered_map<RenderGraphPoolResourceKey, TextureAllocation, ResourceKeyHash> PersistentTextures;
        std::unordered_map<RenderGraphPoolResourceKey, BufferAllocation, ResourceKeyHash> PersistentBuffers;
        std::vector<RetiredResource> RetiredResources;
    };

    RenderGraphResourcePool::RenderGraphResourcePool(nvrhi::IDevice* device, uint32_t frameSlotCount)
        : mImpl(std::make_unique<Impl>(device, frameSlotCount))
    {
        if (!device)
            throw std::invalid_argument("RenderGraphResourcePool requires a valid NVRHI device");
        if (frameSlotCount == 0)
            throw std::invalid_argument("RenderGraphResourcePool requires at least one frame slot");
    }

    RenderGraphResourcePool::~RenderGraphResourcePool() = default;

    void RenderGraphResourcePool::BeginFrame(uint32_t frameSlot, uint64_t completedSubmission)
    {
        Collect(completedSubmission);
        if (std::any_of(
            mImpl->FrameSlots.begin(),
            mImpl->FrameSlots.end(),
            [](const Impl::FrameSlot& candidate) { return candidate.State.bRecording; }))
        {
            throw RenderGraphValidationError(
                "RenderGraphResourcePool supports one active command-recording frame at a time.");
        }
        Impl::FrameSlot& slot = mImpl->RequireFrameSlot(frameSlot);
        if (!slot.State.CanBegin(mImpl->CompletedSubmission))
        {
            throw RenderGraphValidationError(
                "RenderGraph frame slot is still recording or in flight (slot " +
                std::to_string(frameSlot) + ", last submission " +
                std::to_string(slot.State.LastSubmission) + ", completed " +
                std::to_string(mImpl->CompletedSubmission) + ").");
        }

        slot.State.bRecording = true;
        slot.CompletedSubmissionAtBegin = mImpl->CompletedSubmission;
        slot.TextureCursor = 0;
        slot.BufferCursor = 0;
        slot.PersistentTexturesUsed.clear();
        slot.PersistentBuffersUsed.clear();
        slot.AllocationIntervals.clear();
    }

    void RenderGraphResourcePool::EndFrame(uint32_t frameSlot, uint64_t submission)
    {
        Impl::FrameSlot& slot = mImpl->RequireFrameSlot(frameSlot);
        if (!slot.State.bRecording)
            throw RenderGraphValidationError("RenderGraph frame slot ended without BeginFrame.");
        if (submission == 0)
            throw RenderGraphValidationError("RenderGraph frame submission must be non-zero.");
        // A queue-global completed value may be newer than this slot's newly
        // assigned submission instance (for example after WaitForIdle and graph
        // rebuild). That is safe: the frame is already known complete. The only
        // invalid ordering is moving this slot backwards relative to itself.
        if (submission < slot.State.LastSubmission)
            throw RenderGraphValidationError("RenderGraph frame submission moved backwards for its frame slot.");

        for (size_t index = 0; index < slot.TextureCursor; ++index)
            slot.Textures[index].LastSubmission = submission;
        for (size_t index = 0; index < slot.BufferCursor; ++index)
            slot.Buffers[index].LastSubmission = submission;
        for (const RenderGraphPoolResourceKey& key : slot.PersistentTexturesUsed)
        {
            if (auto found = mImpl->PersistentTextures.find(key); found != mImpl->PersistentTextures.end())
                found->second.LastSubmission = submission;
        }
        for (const RenderGraphPoolResourceKey& key : slot.PersistentBuffersUsed)
        {
            if (auto found = mImpl->PersistentBuffers.find(key); found != mImpl->PersistentBuffers.end())
                found->second.LastSubmission = submission;
        }

        slot.State.LastSubmission = submission;
        slot.State.bRecording = false;
    }

    void RenderGraphResourcePool::AbortFrame(uint32_t frameSlot) noexcept
    {
        if (frameSlot >= mImpl->FrameSlots.size())
            return;
        Impl::FrameSlot& slot = mImpl->FrameSlots[frameSlot];
        slot.State.bRecording = false;
        slot.TextureCursor = 0;
        slot.BufferCursor = 0;
        slot.PersistentTexturesUsed.clear();
        slot.PersistentBuffersUsed.clear();
        slot.AllocationIntervals.clear();
    }

    void RenderGraphResourcePool::Collect(uint64_t completedSubmission)
    {
        mImpl->CompletedSubmission = std::max(mImpl->CompletedSubmission, completedSubmission);
        std::erase_if(mImpl->RetiredResources, [this](const Impl::RetiredResource& resource)
        {
            return resource.Submission <= mImpl->CompletedSubmission;
        });
        mImpl->Device->runGarbageCollection();
    }

    uint32_t RenderGraphResourcePool::GetFrameSlotCount() const noexcept
    {
        return static_cast<uint32_t>(mImpl->FrameSlots.size());
    }

    nvrhi::IDevice* RenderGraphResourcePool::GetDevice() const noexcept
    {
        return mImpl->Device;
    }

    bool RenderGraphResourcePool::IsFrameActive(uint32_t frameSlot) const noexcept
    {
        return frameSlot < mImpl->FrameSlots.size() && mImpl->FrameSlots[frameSlot].State.bRecording;
    }

    RenderGraphFrameSlotState RenderGraphResourcePool::GetFrameSlotState(uint32_t frameSlot) const
    {
        return mImpl->RequireFrameSlot(frameSlot).State;
    }

    const std::vector<RenderGraphPoolAllocationInterval>&
        RenderGraphResourcePool::GetAllocationIntervals(uint32_t frameSlot) const
    {
        return mImpl->RequireFrameSlot(frameSlot).AllocationIntervals;
    }

    RenderGraphResourcePoolStats RenderGraphResourcePool::GetStats() const noexcept
    {
        RenderGraphResourcePoolStats stats;
        stats.PersistentTextureCount = mImpl->PersistentTextures.size();
        stats.PersistentBufferCount = mImpl->PersistentBuffers.size();
        for (const auto& [key, allocation] : mImpl->PersistentTextures)
        {
            (void)key;
            stats.PersistentTextureBytes += EstimateTextureBytes(allocation.Desc);
        }
        for (const auto& [key, allocation] : mImpl->PersistentBuffers)
        {
            (void)key;
            stats.PersistentBufferBytes += allocation.Desc.ByteSize;
        }
        for (const Impl::FrameSlot& slot : mImpl->FrameSlots)
        {
            stats.TransientTextureCount += slot.Textures.size();
            stats.TransientBufferCount += slot.Buffers.size();
            uint64_t frameBytes = 0;
            for (const Impl::TextureAllocation& allocation : slot.Textures)
            {
                const uint64_t bytes = EstimateTextureBytes(allocation.Desc);
                stats.TransientTextureBytes += bytes;
                frameBytes += bytes;
            }
            for (const Impl::BufferAllocation& allocation : slot.Buffers)
            {
                stats.TransientBufferBytes += allocation.Desc.ByteSize;
                frameBytes += allocation.Desc.ByteSize;
            }
            stats.PeakFrameTransientBytes = std::max(
                stats.PeakFrameTransientBytes,
                frameBytes);
        }
        stats.RetiredResourceCount = mImpl->RetiredResources.size();
        return stats;
    }

    nvrhi::TextureHandle RenderGraphResourcePool::AcquireTexture(
        uint32_t frameSlot,
        const RenderGraphPoolResourceKey& key,
        const RenderGraphTextureDesc& desc,
        RenderGraphResourceLifetime lifetime,
        RenderGraphUsage requiredUsage,
        RenderGraphResourceLifetimeInfo passInterval)
    {
        Impl::FrameSlot& slot = mImpl->RequireFrameSlot(frameSlot);
        if (!slot.State.bRecording)
            throw RenderGraphValidationError("RenderGraph resource realization requires an active frame slot.");
        if (lifetime == RenderGraphResourceLifetime::Imported)
            throw RenderGraphValidationError("Imported RenderGraph textures are not allocated by the resource pool.");

        const RenderGraphUsage capabilities = TextureCapabilities(
            requiredUsage | desc.CapabilityUsage);
        Impl::TextureAllocation* allocation = nullptr;
        if (lifetime == RenderGraphResourceLifetime::Persistent)
        {
            auto [found, inserted] = mImpl->PersistentTextures.try_emplace(key);
            allocation = &found->second;
            if (inserted || !SameTextureShape(allocation->Desc, desc) ||
                !IncludesUsage(allocation->Capabilities, capabilities))
            {
                if (!inserted)
                    mImpl->Retire(std::move(allocation->Resource), allocation->LastSubmission);
                allocation->Desc = desc;
                allocation->Capabilities = capabilities;
                allocation->Resource = mImpl->Device->createTexture(
                    MakeTextureDesc(desc, capabilities, frameSlot, false));
                allocation->PhysicalAllocationId = mImpl->AllocatePhysicalId();
                allocation->LastSubmission = 0;
                if (!allocation->Resource)
                    throw RenderGraphValidationError("NVRHI failed to create RenderGraph texture: " + desc.DebugName);
            }
            slot.PersistentTexturesUsed.push_back(key);
        }
        else
        {
            const size_t cursor = slot.TextureCursor;
            auto compatible = std::find_if(
                slot.Textures.begin() + static_cast<std::ptrdiff_t>(cursor),
                slot.Textures.end(),
                [&desc, capabilities](const Impl::TextureAllocation& candidate)
                {
                    return SameTextureShape(candidate.Desc, desc) &&
                        IncludesUsage(candidate.Capabilities, capabilities);
                });
            if (compatible != slot.Textures.end() && compatible != slot.Textures.begin() + static_cast<std::ptrdiff_t>(cursor))
                std::iter_swap(compatible, slot.Textures.begin() + static_cast<std::ptrdiff_t>(cursor));

            if (cursor == slot.Textures.size())
                slot.Textures.emplace_back();
            allocation = &slot.Textures[cursor];
            if (!allocation->Resource || !SameTextureShape(allocation->Desc, desc) ||
                !IncludesUsage(allocation->Capabilities, capabilities))
            {
                mImpl->Retire(std::move(allocation->Resource), allocation->LastSubmission);
                allocation->Desc = desc;
                allocation->Capabilities = capabilities;
                allocation->Resource = mImpl->Device->createTexture(
                    MakeTextureDesc(desc, capabilities, frameSlot, true));
                allocation->PhysicalAllocationId = mImpl->AllocatePhysicalId();
                allocation->LastSubmission = 0;
                if (!allocation->Resource)
                    throw RenderGraphValidationError("NVRHI failed to create transient RenderGraph texture: " + desc.DebugName);
            }
            ++slot.TextureCursor;
        }

        slot.AllocationIntervals.push_back({
            .Resource = key,
            .Lifetime = lifetime,
            .PassInterval = passInterval,
            .PhysicalAllocationId = allocation->PhysicalAllocationId
        });
        return allocation->Resource;
    }

    nvrhi::BufferHandle RenderGraphResourcePool::AcquireBuffer(
        uint32_t frameSlot,
        const RenderGraphPoolResourceKey& key,
        const RenderGraphBufferDesc& desc,
        RenderGraphResourceLifetime lifetime,
        RenderGraphUsage requiredUsage,
        RenderGraphResourceLifetimeInfo passInterval)
    {
        Impl::FrameSlot& slot = mImpl->RequireFrameSlot(frameSlot);
        if (!slot.State.bRecording)
            throw RenderGraphValidationError("RenderGraph resource realization requires an active frame slot.");
        if (lifetime == RenderGraphResourceLifetime::Imported)
            throw RenderGraphValidationError("Imported RenderGraph buffers are not allocated by the resource pool.");

        const RenderGraphUsage capabilities = BufferCapabilities(requiredUsage);
        Impl::BufferAllocation* allocation = nullptr;
        if (lifetime == RenderGraphResourceLifetime::Persistent)
        {
            auto [found, inserted] = mImpl->PersistentBuffers.try_emplace(key);
            allocation = &found->second;
            if (inserted || !SameBufferShape(allocation->Desc, desc) ||
                !IncludesUsage(allocation->Capabilities, capabilities))
            {
                if (!inserted)
                    mImpl->Retire(std::move(allocation->Resource), allocation->LastSubmission);
                allocation->Desc = desc;
                allocation->Capabilities = capabilities;
                allocation->Resource = mImpl->Device->createBuffer(
                    MakeBufferDesc(desc, capabilities, frameSlot, false));
                allocation->PhysicalAllocationId = mImpl->AllocatePhysicalId();
                allocation->LastSubmission = 0;
                if (!allocation->Resource)
                    throw RenderGraphValidationError("NVRHI failed to create RenderGraph buffer: " + desc.DebugName);
            }
            slot.PersistentBuffersUsed.push_back(key);
        }
        else
        {
            const size_t cursor = slot.BufferCursor;
            auto compatible = std::find_if(
                slot.Buffers.begin() + static_cast<std::ptrdiff_t>(cursor),
                slot.Buffers.end(),
                [&desc, capabilities](const Impl::BufferAllocation& candidate)
                {
                    return SameBufferShape(candidate.Desc, desc) &&
                        IncludesUsage(candidate.Capabilities, capabilities);
                });
            if (compatible != slot.Buffers.end() && compatible != slot.Buffers.begin() + static_cast<std::ptrdiff_t>(cursor))
                std::iter_swap(compatible, slot.Buffers.begin() + static_cast<std::ptrdiff_t>(cursor));

            if (cursor == slot.Buffers.size())
                slot.Buffers.emplace_back();
            allocation = &slot.Buffers[cursor];
            if (!allocation->Resource || !SameBufferShape(allocation->Desc, desc) ||
                !IncludesUsage(allocation->Capabilities, capabilities))
            {
                mImpl->Retire(std::move(allocation->Resource), allocation->LastSubmission);
                allocation->Desc = desc;
                allocation->Capabilities = capabilities;
                allocation->Resource = mImpl->Device->createBuffer(
                    MakeBufferDesc(desc, capabilities, frameSlot, true));
                allocation->PhysicalAllocationId = mImpl->AllocatePhysicalId();
                allocation->LastSubmission = 0;
                if (!allocation->Resource)
                    throw RenderGraphValidationError("NVRHI failed to create transient RenderGraph buffer: " + desc.DebugName);
            }
            ++slot.BufferCursor;
        }

        slot.AllocationIntervals.push_back({
            .Resource = key,
            .Lifetime = lifetime,
            .PassInterval = passInterval,
            .PhysicalAllocationId = allocation->PhysicalAllocationId
        });
        return allocation->Resource;
    }

    void RenderGraphResourcePool::ReleasePersistentResources(uint64_t graphNamespace)
    {
        for (auto iterator = mImpl->PersistentTextures.begin();
            iterator != mImpl->PersistentTextures.end();)
        {
            if (iterator->first.GraphNamespace != graphNamespace)
            {
                ++iterator;
                continue;
            }
            mImpl->Retire(std::move(iterator->second.Resource), iterator->second.LastSubmission);
            iterator = mImpl->PersistentTextures.erase(iterator);
        }
        for (auto iterator = mImpl->PersistentBuffers.begin();
            iterator != mImpl->PersistentBuffers.end();)
        {
            if (iterator->first.GraphNamespace != graphNamespace)
            {
                ++iterator;
                continue;
            }
            mImpl->Retire(std::move(iterator->second.Resource), iterator->second.LastSubmission);
            iterator = mImpl->PersistentBuffers.erase(iterator);
        }
    }
}
