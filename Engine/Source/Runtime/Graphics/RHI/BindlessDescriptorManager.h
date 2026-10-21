#pragma once

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace HEngine
{
    inline constexpr uint32_t InvalidDescriptorIndex = std::numeric_limits<uint32_t>::max();

    struct DescriptorAllocationHandle
    {
        uint32_t Index = InvalidDescriptorIndex;
        uint32_t Generation = 0;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return Index != InvalidDescriptorIndex && Generation != 0;
        }

        [[nodiscard]] bool operator==(const DescriptorAllocationHandle&) const = default;
    };

    // Device-independent descriptor index state. Release immediately invalidates
    // the handle, but the numeric index remains quarantined until its GPU
    // submission completes.
    class DescriptorIndexAllocator final
    {
    public:
        explicit DescriptorIndexAllocator(uint32_t capacity, uint32_t reservedCount = 0);

        [[nodiscard]] DescriptorAllocationHandle Allocate();
        void Release(DescriptorAllocationHandle handle, uint64_t retireAfterSubmission);
        void Collect(uint64_t completedSubmission);

        [[nodiscard]] bool IsValid(DescriptorAllocationHandle handle) const noexcept;
        [[nodiscard]] uint32_t GetCapacity() const noexcept { return mCapacity; }
        [[nodiscard]] uint32_t GetReservedCount() const noexcept { return mReservedCount; }
        [[nodiscard]] size_t GetLiveCount() const noexcept { return mLiveCount; }
        [[nodiscard]] size_t GetRetiredCount() const noexcept { return mRetired.size(); }
        [[nodiscard]] size_t GetAvailableCount() const noexcept { return mFreeIndices.size(); }

    private:
        struct Slot
        {
            uint32_t Generation = 1;
            bool bLive = false;
            bool bRetired = false;
        };

        struct RetiredIndex
        {
            uint32_t Index = InvalidDescriptorIndex;
            uint64_t Submission = 0;
        };

        uint32_t mCapacity = 0;
        uint32_t mReservedCount = 0;
        std::vector<Slot> mSlots;
        std::vector<uint32_t> mFreeIndices;
        std::vector<RetiredIndex> mRetired;
        size_t mLiveCount = 0;
    };

    struct BindlessTextureHandle
    {
        uint32_t Index = InvalidDescriptorIndex;
        uint32_t Generation = 0;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return Index != InvalidDescriptorIndex && Generation != 0;
        }

        [[nodiscard]] bool operator==(const BindlessTextureHandle&) const = default;
    };

    enum class DefaultBindlessTexture : uint8_t
    {
        White,
        Black,
        Normal,
    };

    enum class ShaderDescriptorAccess : uint8_t
    {
        BoundedTable,
        HeapDirectlyIndexed,
    };

    // Tier 1 exposes 128 SRVs and 16 samplers per stage. SceneRenderer reserves
    // the remainder for GPU-scene, shadow and environment bindings.
    inline constexpr uint32_t BaselineMaterialTextureDescriptorCount = 120;
    inline constexpr uint32_t BaselineMaterialSamplerDescriptorCount = 15;

    struct BindlessTextureTableDesc
    {
        uint32_t Capacity = BaselineMaterialTextureDescriptorCount;
        nvrhi::ShaderType Visibility = nvrhi::ShaderType::All;
        ShaderDescriptorAccess Access = ShaderDescriptorAccess::BoundedTable;
        std::string DebugName = "GlobalBindlessTextures";
    };

    struct BindlessSamplerHandle
    {
        uint32_t Index = InvalidDescriptorIndex;
        uint32_t Generation = 0;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return Index != InvalidDescriptorIndex && Generation != 0;
        }

        [[nodiscard]] bool operator==(const BindlessSamplerHandle&) const = default;
    };

    struct BindlessSamplerTableDesc
    {
        uint32_t Capacity = BaselineMaterialSamplerDescriptorCount;
        nvrhi::ShaderType Visibility = nvrhi::ShaderType::All;
        // Bounded sampler tables use a distinct descriptor-set register space
        // from bounded texture tables. Heap-direct shaders ignore this value.
        uint32_t RegisterSpace = 4;
        ShaderDescriptorAccess Access = ShaderDescriptorAccess::BoundedTable;
        std::string DebugName = "GlobalBindlessSamplers";
    };

    // Owns the global Texture_SRV descriptor table. Mutable sampler descriptors
    // use a different hardware heap in DX12 and therefore require a separate
    // NVRHI MutableSampler layout/table; samplers are intentionally not hidden
    // inside this texture-only table.
    class BindlessTextureTable final
    {
    public:
        BindlessTextureTable(nvrhi::IDevice* device, const BindlessTextureTableDesc& desc);
        ~BindlessTextureTable() = default;

        BindlessTextureTable(const BindlessTextureTable&) = delete;
        BindlessTextureTable& operator=(const BindlessTextureTable&) = delete;

        [[nodiscard]] BindlessTextureHandle AllocateTexture(
            const nvrhi::TextureHandle& texture,
            nvrhi::TextureSubresourceSet subresources = nvrhi::AllSubresources,
            nvrhi::Format format = nvrhi::Format::UNKNOWN,
            nvrhi::TextureDimension dimension = nvrhi::TextureDimension::Unknown);
        // NVRHI descriptor-table writes are immediate. Update in a frame setup
        // phase only after prior readers completed; otherwise allocate a new
        // handle and retire the old one at its last submission.
        void UpdateTexture(
            BindlessTextureHandle handle,
            const nvrhi::TextureHandle& texture,
            nvrhi::TextureSubresourceSet subresources = nvrhi::AllSubresources,
            nvrhi::Format format = nvrhi::Format::UNKNOWN,
            nvrhi::TextureDimension dimension = nvrhi::TextureDimension::Unknown);
        // Publication may change the physical handle. Heap-direct readers retain
        // the old descriptor through their last submission. False means a full
        // heap still has readers in flight; retry after collection, without mutation.
        [[nodiscard]] bool TryPublishTexture(BindlessTextureHandle& handle,
            const nvrhi::TextureHandle& texture, uint64_t lastSubmittedSubmission,
            uint64_t completedSubmission);
        void ReleaseTexture(BindlessTextureHandle handle, uint64_t retireAfterSubmission);
        void Collect(uint64_t completedSubmission);

        // Defaults reserve stable indices 0..2 and must be initialized before
        // materials consume them. They cannot be released or recycled.
        void SetDefaultTexture(
            DefaultBindlessTexture textureKind,
            const nvrhi::TextureHandle& texture,
            nvrhi::TextureSubresourceSet subresources = nvrhi::AllSubresources,
            nvrhi::Format format = nvrhi::Format::UNKNOWN,
            nvrhi::TextureDimension dimension = nvrhi::TextureDimension::Unknown);
        [[nodiscard]] bool HasDefaultTexture(DefaultBindlessTexture textureKind) const noexcept;
        // Returns either a table-relative index for baseline shaders or an
        // absolute ResourceDescriptorHeap index for heap-direct shaders.
        [[nodiscard]] uint32_t GetDefaultTextureIndex(DefaultBindlessTexture textureKind) const;

        [[nodiscard]] bool IsValid(BindlessTextureHandle handle) const noexcept;
        // The returned index follows the access mode selected at construction.
        [[nodiscard]] uint32_t GetShaderDescriptorIndex(BindlessTextureHandle handle) const;
        [[nodiscard]] uint32_t GetCapacity() const noexcept { return mAllocator.GetCapacity(); }
        [[nodiscard]] size_t GetLiveTextureCount() const noexcept { return mAllocator.GetLiveCount(); }
        [[nodiscard]] size_t GetRetiredTextureCount() const noexcept { return mAllocator.GetRetiredCount(); }

        [[nodiscard]] const nvrhi::BindingLayoutHandle& GetBindingLayout() const noexcept { return mLayout; }
        [[nodiscard]] const nvrhi::DescriptorTableHandle& GetDescriptorTable() const noexcept { return mTable; }
        [[nodiscard]] nvrhi::IBindingSet* GetBindingSet() const noexcept;

    private:
        static constexpr uint32_t DefaultTextureCount = 3;

        struct RetainedTexture
        {
            uint32_t Index = InvalidDescriptorIndex;
            uint64_t Submission = 0;
            nvrhi::TextureHandle Texture;
        };

        struct TextureView
        {
            nvrhi::TextureSubresourceSet Subresources = nvrhi::AllSubresources;
            nvrhi::Format Format = nvrhi::Format::UNKNOWN;
            nvrhi::TextureDimension Dimension = nvrhi::TextureDimension::Unknown;
        };

        [[nodiscard]] static uint32_t GetDefaultIndex(DefaultBindlessTexture textureKind);
        void WriteTexture(
            uint32_t index,
            const nvrhi::TextureHandle& texture,
            nvrhi::TextureSubresourceSet subresources,
            nvrhi::Format format,
            nvrhi::TextureDimension dimension);
        void ClearDescriptor(uint32_t index);
        void RebuildBoundedBindingSet();

        nvrhi::DeviceHandle mDevice;
        DescriptorIndexAllocator mAllocator;
        ShaderDescriptorAccess mAccess = ShaderDescriptorAccess::BoundedTable;
        nvrhi::BindingLayoutHandle mLayout;
        nvrhi::DescriptorTableHandle mTable;
        nvrhi::BindingSetHandle mBoundedBindingSet;
        uint32_t mShaderDescriptorBase = 0;
        std::vector<nvrhi::TextureHandle> mTextures;
        std::vector<TextureView> mTextureViews;
        std::vector<RetainedTexture> mRetiredTextures;
        bool mDefaultInitialized[DefaultTextureCount] = { false, false, false };
    };

    // Samplers live in their own table because DX12 has a distinct shader-visible
    // sampler heap. This mirrors NVRHI's MutableSampler layout instead of making
    // a texture handle ambiguously address two descriptor heaps.
    class BindlessSamplerTable final
    {
    public:
        BindlessSamplerTable(nvrhi::IDevice* device, const BindlessSamplerTableDesc& desc);
        ~BindlessSamplerTable() = default;

        BindlessSamplerTable(const BindlessSamplerTable&) = delete;
        BindlessSamplerTable& operator=(const BindlessSamplerTable&) = delete;

        [[nodiscard]] BindlessSamplerHandle AllocateSampler(const nvrhi::SamplerHandle& sampler);
        // Like texture writes, sampler descriptor updates require synchronization
        // with any in-flight shader reads of the same index.
        void UpdateSampler(BindlessSamplerHandle handle, const nvrhi::SamplerHandle& sampler);
        void ReleaseSampler(BindlessSamplerHandle handle, uint64_t retireAfterSubmission);
        void Collect(uint64_t completedSubmission);

        [[nodiscard]] bool IsValid(BindlessSamplerHandle handle) const noexcept;
        [[nodiscard]] uint32_t GetShaderDescriptorIndex(BindlessSamplerHandle handle) const;
        [[nodiscard]] uint32_t GetCapacity() const noexcept { return mAllocator.GetCapacity(); }
        [[nodiscard]] size_t GetLiveSamplerCount() const noexcept { return mAllocator.GetLiveCount(); }
        [[nodiscard]] size_t GetRetiredSamplerCount() const noexcept { return mAllocator.GetRetiredCount(); }

        [[nodiscard]] const nvrhi::BindingLayoutHandle& GetBindingLayout() const noexcept { return mLayout; }
        [[nodiscard]] const nvrhi::DescriptorTableHandle& GetDescriptorTable() const noexcept { return mTable; }
        [[nodiscard]] nvrhi::IBindingSet* GetBindingSet() const noexcept;

    private:
        struct RetainedSampler
        {
            uint32_t Index = InvalidDescriptorIndex;
            uint64_t Submission = 0;
            nvrhi::SamplerHandle Sampler;
        };

        void WriteSampler(uint32_t index, const nvrhi::SamplerHandle& sampler);
        void RebuildBoundedBindingSet();

        nvrhi::DeviceHandle mDevice;
        DescriptorIndexAllocator mAllocator;
        ShaderDescriptorAccess mAccess = ShaderDescriptorAccess::BoundedTable;
        nvrhi::BindingLayoutHandle mLayout;
        nvrhi::DescriptorTableHandle mTable;
        nvrhi::BindingSetHandle mBoundedBindingSet;
        uint32_t mShaderDescriptorBase = 0;
        std::vector<nvrhi::SamplerHandle> mSamplers;
        std::vector<RetainedSampler> mRetiredSamplers;
    };

    struct BindlessDescriptorManagerDesc
    {
        BindlessTextureTableDesc Textures;
        BindlessSamplerTableDesc Samplers;
    };

    class BindlessDescriptorManager final
    {
    public:
        BindlessDescriptorManager(
            nvrhi::IDevice* device,
            const BindlessDescriptorManagerDesc& desc = {});
        ~BindlessDescriptorManager() = default;

        BindlessDescriptorManager(const BindlessDescriptorManager&) = delete;
        BindlessDescriptorManager& operator=(const BindlessDescriptorManager&) = delete;

        void Collect(uint64_t completedSubmission);

        [[nodiscard]] BindlessTextureTable& GetTextures() noexcept { return mTextures; }
        [[nodiscard]] const BindlessTextureTable& GetTextures() const noexcept { return mTextures; }
        [[nodiscard]] BindlessSamplerTable& GetSamplers() noexcept { return mSamplers; }
        [[nodiscard]] const BindlessSamplerTable& GetSamplers() const noexcept { return mSamplers; }

    private:
        BindlessTextureTable mTextures;
        BindlessSamplerTable mSamplers;
    };
}
