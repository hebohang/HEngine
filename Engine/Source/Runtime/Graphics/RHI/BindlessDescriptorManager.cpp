#include "Hepch.h"

#include "BindlessDescriptorManager.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] uint32_t NextGeneration(uint32_t generation) noexcept
        {
            ++generation;
            return generation == 0 ? 1 : generation;
        }

        [[nodiscard]] DescriptorAllocationHandle ToAllocationHandle(BindlessTextureHandle handle) noexcept
        {
            return { handle.Index, handle.Generation };
        }
    }

    DescriptorIndexAllocator::DescriptorIndexAllocator(uint32_t capacity, uint32_t reservedCount)
        : mCapacity(capacity), mReservedCount(reservedCount), mSlots(capacity)
    {
        if (capacity == 0)
            throw std::invalid_argument("Descriptor index allocator capacity must be non-zero");
        if (reservedCount > capacity)
            throw std::invalid_argument("Descriptor index allocator reserved count exceeds capacity");

        mFreeIndices.reserve(capacity - reservedCount);
        for (uint32_t index = capacity; index > reservedCount; --index)
            mFreeIndices.push_back(index - 1);
    }

    DescriptorAllocationHandle DescriptorIndexAllocator::Allocate()
    {
        if (mFreeIndices.empty())
        {
            throw std::runtime_error(
                "Bindless descriptor capacity exhausted (capacity=" + std::to_string(mCapacity) +
                ", reserved=" + std::to_string(mReservedCount) +
                ", live=" + std::to_string(mLiveCount) +
                ", retired=" + std::to_string(mRetired.size()) + ")");
        }

        const uint32_t index = mFreeIndices.back();
        mFreeIndices.pop_back();
        Slot& slot = mSlots[index];
        if (slot.bLive || slot.bRetired)
            throw std::logic_error("Descriptor index allocator attempted to reuse an unavailable slot");
        slot.bLive = true;
        ++mLiveCount;
        return { index, slot.Generation };
    }

    void DescriptorIndexAllocator::Release(
        DescriptorAllocationHandle handle,
        uint64_t retireAfterSubmission)
    {
        if (!IsValid(handle))
            throw std::out_of_range("Invalid or stale bindless descriptor handle");

        Slot& slot = mSlots[handle.Index];
        slot.bLive = false;
        slot.bRetired = true;
        slot.Generation = NextGeneration(slot.Generation);
        --mLiveCount;

        if (retireAfterSubmission == 0)
        {
            slot.bRetired = false;
            mFreeIndices.push_back(handle.Index);
            return;
        }

        mRetired.push_back({ handle.Index, retireAfterSubmission });
    }

    void DescriptorIndexAllocator::Collect(uint64_t completedSubmission)
    {
        for (size_t index = 0; index < mRetired.size();)
        {
            const RetiredIndex retired = mRetired[index];
            if (retired.Submission > completedSubmission)
            {
                ++index;
                continue;
            }

            Slot& slot = mSlots[retired.Index];
            if (!slot.bRetired || slot.bLive)
                throw std::logic_error("Descriptor index retirement state is inconsistent");
            slot.bRetired = false;
            mFreeIndices.push_back(retired.Index);
            mRetired[index] = mRetired.back();
            mRetired.pop_back();
        }
    }

    bool DescriptorIndexAllocator::IsValid(DescriptorAllocationHandle handle) const noexcept
    {
        return handle &&
            handle.Index >= mReservedCount &&
            handle.Index < mSlots.size() &&
            mSlots[handle.Index].bLive &&
            mSlots[handle.Index].Generation == handle.Generation;
    }

    BindlessTextureTable::BindlessTextureTable(
        nvrhi::IDevice* device,
        const BindlessTextureTableDesc& desc)
        : mDevice(device)
        , mAllocator(desc.Capacity, DefaultTextureCount)
        , mAccess(desc.Access)
        , mTextures(desc.Capacity)
        , mTextureViews(desc.Capacity)
    {
        if (!mDevice)
            throw std::invalid_argument("BindlessTextureTable requires a valid NVRHI device");
        if (desc.Capacity <= DefaultTextureCount)
            throw std::invalid_argument("BindlessTextureTable capacity must exceed the three reserved defaults");
        if (desc.Visibility == nvrhi::ShaderType::None)
            throw std::invalid_argument("BindlessTextureTable shader visibility must not be None");

        if (mAccess == ShaderDescriptorAccess::HeapDirectlyIndexed)
        {
            nvrhi::BindlessLayoutDesc layoutDesc;
            layoutDesc.visibility = desc.Visibility;
            layoutDesc.firstSlot = 0;
            layoutDesc.maxCapacity = desc.Capacity;
            layoutDesc.layoutType = nvrhi::BindlessLayoutDesc::LayoutType::MutableSrvUavCbv;
            mLayout = mDevice->createBindlessLayout(layoutDesc);
        }
        else
        {
            if (desc.Capacity > std::numeric_limits<uint16_t>::max())
                throw std::invalid_argument("Bounded texture descriptor capacity exceeds the NVRHI layout limit");
            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.visibility = desc.Visibility;
            layoutDesc.setRegisterSpaceAndDescriptorSet(1);
            layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0).setSize(desc.Capacity));
            mLayout = mDevice->createBindingLayout(layoutDesc);
        }
        if (!mLayout)
        {
            throw std::runtime_error(
                "NVRHI failed to create texture descriptor layout '" + desc.DebugName + "'");
        }

        if (mAccess == ShaderDescriptorAccess::HeapDirectlyIndexed)
        {
            mTable = mDevice->createDescriptorTable(mLayout);
            if (!mTable)
            {
                throw std::runtime_error(
                    "NVRHI failed to create heap-direct texture descriptor table '" +
                    desc.DebugName + "'");
            }

            mDevice->resizeDescriptorTable(mTable, desc.Capacity, false);
            if (mTable->getCapacity() != desc.Capacity)
            {
                throw std::runtime_error(
                    "NVRHI heap-direct texture table capacity does not match the request for '" +
                    desc.DebugName + "' (requested=" + std::to_string(desc.Capacity) +
                    ", actual=" + std::to_string(mTable->getCapacity()) + ")");
            }
            mShaderDescriptorBase = mTable->getFirstDescriptorIndexInHeap();
        }
    }

    BindlessTextureHandle BindlessTextureTable::AllocateTexture(
        const nvrhi::TextureHandle& texture,
        nvrhi::TextureSubresourceSet subresources,
        nvrhi::Format format,
        nvrhi::TextureDimension dimension)
    {
        if (!texture)
            throw std::invalid_argument("Cannot allocate a bindless descriptor for a null texture");

        DescriptorAllocationHandle allocation;
        try
        {
            allocation = mAllocator.Allocate();
        }
        catch (const std::runtime_error& exception)
        {
            throw std::runtime_error(
                std::string(mAccess == ShaderDescriptorAccess::BoundedTable
                    ? "Baseline bounded material texture table is full; reduce simultaneously resident textures or add descriptor paging. "
                    : "Heap-direct material texture table is full. ") +
                exception.what());
        }
        mTextures[allocation.Index] = texture;
        mTextureViews[allocation.Index] = { subresources, format, dimension };
        try
        {
            WriteTexture(allocation.Index, texture, subresources, format, dimension);
        }
        catch (...)
        {
            mTextures[allocation.Index] = nullptr;
            mTextureViews[allocation.Index] = {};
            mAllocator.Release(allocation, 0);
            throw;
        }
        return { allocation.Index, allocation.Generation };
    }

    void BindlessTextureTable::UpdateTexture(
        BindlessTextureHandle handle,
        const nvrhi::TextureHandle& texture,
        nvrhi::TextureSubresourceSet subresources,
        nvrhi::Format format,
        nvrhi::TextureDimension dimension)
    {
        if (!IsValid(handle))
            throw std::out_of_range("Cannot update an invalid or stale bindless texture handle");
        if (!texture)
            throw std::invalid_argument("Cannot update a bindless descriptor with a null texture");

        const nvrhi::TextureHandle previousTexture = mTextures[handle.Index];
        const TextureView previousView = mTextureViews[handle.Index];
        mTextures[handle.Index] = texture;
        mTextureViews[handle.Index] = { subresources, format, dimension };
        try
        {
            WriteTexture(handle.Index, texture, subresources, format, dimension);
        }
        catch (...)
        {
            mTextures[handle.Index] = previousTexture;
            mTextureViews[handle.Index] = previousView;
            if (mAccess == ShaderDescriptorAccess::BoundedTable)
                RebuildBoundedBindingSet();
            throw;
        }
    }

    bool BindlessTextureTable::TryPublishTexture(BindlessTextureHandle& handle,
        const nvrhi::TextureHandle& texture, uint64_t lastSubmittedSubmission,
        uint64_t completedSubmission)
    {
        if (!IsValid(handle))
            throw std::out_of_range("Cannot publish an invalid or stale bindless texture handle");
        if (!texture)
            throw std::invalid_argument("Cannot publish a null texture");
        const TextureView view = mTextureViews[handle.Index];
        if (mAccess == ShaderDescriptorAccess::BoundedTable ||
            completedSubmission >= lastSubmittedSubmission)
        {
            // Bounded mode creates a new immutable binding set; command lists
            // retain their original set. Heap-direct overwrite is safe only idle.
            UpdateTexture(handle, texture, view.Subresources, view.Format, view.Dimension);
            return true;
        }
        if (mAllocator.GetLiveCount() + mAllocator.GetRetiredCount() + DefaultTextureCount >=
            mAllocator.GetCapacity())
            return false;
        const auto replacement = AllocateTexture(texture, view.Subresources, view.Format, view.Dimension);
        ReleaseTexture(handle, lastSubmittedSubmission);
        handle = replacement;
        return true;
    }

    void BindlessTextureTable::ReleaseTexture(
        BindlessTextureHandle handle,
        uint64_t retireAfterSubmission)
    {
        if (!IsValid(handle))
            throw std::out_of_range("Cannot release an invalid or stale bindless texture handle");

        const nvrhi::TextureHandle retainedTexture = mTextures[handle.Index];
        const TextureView retainedView = mTextureViews[handle.Index];
        if (mAccess == ShaderDescriptorAccess::BoundedTable)
        {
            mTextures[handle.Index] = nullptr;
            mTextureViews[handle.Index] = {};
            try
            {
                RebuildBoundedBindingSet();
            }
            catch (...)
            {
                mTextures[handle.Index] = retainedTexture;
                // The prior view is still valid because the slot has not been
                // released from the allocator yet.
                mTextureViews[handle.Index] = retainedView;
                RebuildBoundedBindingSet();
                throw;
            }
        }

        if (retireAfterSubmission == 0)
        {
            if (mAccess == ShaderDescriptorAccess::HeapDirectlyIndexed)
            {
                ClearDescriptor(handle.Index);
                mTextures[handle.Index] = nullptr;
                mTextureViews[handle.Index] = {};
            }
        }
        else
        {
            mRetiredTextures.push_back({ handle.Index, retireAfterSubmission, retainedTexture });
            if (mAccess == ShaderDescriptorAccess::HeapDirectlyIndexed)
            {
                mTextures[handle.Index] = nullptr;
                mTextureViews[handle.Index] = {};
            }
        }
        mAllocator.Release(ToAllocationHandle(handle), retireAfterSubmission);
    }

    void BindlessTextureTable::Collect(uint64_t completedSubmission)
    {
        for (size_t index = 0; index < mRetiredTextures.size();)
        {
            const RetainedTexture& retired = mRetiredTextures[index];
            if (retired.Submission > completedSubmission)
            {
                ++index;
                continue;
            }

            if (mAccess == ShaderDescriptorAccess::HeapDirectlyIndexed)
                ClearDescriptor(retired.Index);
            if (index + 1 != mRetiredTextures.size())
                mRetiredTextures[index] = std::move(mRetiredTextures.back());
            mRetiredTextures.pop_back();
        }
        mAllocator.Collect(completedSubmission);
    }

    void BindlessTextureTable::SetDefaultTexture(
        DefaultBindlessTexture textureKind,
        const nvrhi::TextureHandle& texture,
        nvrhi::TextureSubresourceSet subresources,
        nvrhi::Format format,
        nvrhi::TextureDimension dimension)
    {
        if (!texture)
            throw std::invalid_argument("Cannot initialize a default bindless descriptor with a null texture");

        const uint32_t index = GetDefaultIndex(textureKind);
        mTextures[index] = texture;
        mTextureViews[index] = { subresources, format, dimension };
        WriteTexture(index, texture, subresources, format, dimension);
        mDefaultInitialized[index] = true;
    }

    bool BindlessTextureTable::HasDefaultTexture(DefaultBindlessTexture textureKind) const noexcept
    {
        const uint32_t index = static_cast<uint32_t>(textureKind);
        return index < DefaultTextureCount && mDefaultInitialized[index];
    }

    uint32_t BindlessTextureTable::GetDefaultTextureIndex(DefaultBindlessTexture textureKind) const
    {
        const uint32_t index = GetDefaultIndex(textureKind);
        if (!mDefaultInitialized[index])
            throw std::logic_error("Requested default bindless texture has not been initialized");
        if (mAccess == ShaderDescriptorAccess::BoundedTable)
            return index;
        if (mShaderDescriptorBase > std::numeric_limits<uint32_t>::max() - index)
            throw std::overflow_error("Default bindless shader descriptor index overflow");
        return mShaderDescriptorBase + index;
    }

    bool BindlessTextureTable::IsValid(BindlessTextureHandle handle) const noexcept
    {
        return mAllocator.IsValid(ToAllocationHandle(handle));
    }

    uint32_t BindlessTextureTable::GetShaderDescriptorIndex(BindlessTextureHandle handle) const
    {
        if (!IsValid(handle))
            throw std::out_of_range("Invalid or stale bindless texture handle");
        if (mAccess == ShaderDescriptorAccess::BoundedTable)
            return handle.Index;
        if (mShaderDescriptorBase > std::numeric_limits<uint32_t>::max() - handle.Index)
            throw std::overflow_error("Bindless shader descriptor index overflow");
        return mShaderDescriptorBase + handle.Index;
    }

    uint32_t BindlessTextureTable::GetDefaultIndex(DefaultBindlessTexture textureKind)
    {
        const uint32_t index = static_cast<uint32_t>(textureKind);
        if (index >= DefaultTextureCount)
            throw std::out_of_range("Unknown default bindless texture kind");
        return index;
    }

    void BindlessTextureTable::WriteTexture(
        uint32_t index,
        const nvrhi::TextureHandle& texture,
        nvrhi::TextureSubresourceSet subresources,
        nvrhi::Format format,
        nvrhi::TextureDimension dimension)
    {
        if (mAccess == ShaderDescriptorAccess::BoundedTable)
        {
            RebuildBoundedBindingSet();
            return;
        }

        nvrhi::BindingSetItem item = nvrhi::BindingSetItem::Texture_SRV(
            index,
            texture,
            format,
            subresources,
            dimension);
        if (!mDevice->writeDescriptorTable(mTable, item))
        {
            throw std::runtime_error(
                "NVRHI failed to write bindless texture descriptor index " + std::to_string(index));
        }
    }

    void BindlessTextureTable::ClearDescriptor(uint32_t index)
    {
        if (mAccess == ShaderDescriptorAccess::BoundedTable)
        {
            RebuildBoundedBindingSet();
            return;
        }
        if (!mDevice->writeDescriptorTable(mTable, nvrhi::BindingSetItem::None(index)))
        {
            throw std::runtime_error(
                "NVRHI failed to clear bindless texture descriptor index " + std::to_string(index));
        }
    }

    void BindlessTextureTable::RebuildBoundedBindingSet()
    {
        if (mAccess != ShaderDescriptorAccess::BoundedTable || mTextures.empty() || !mTextures[0])
            return;

        nvrhi::BindingSetDesc setDesc;
        setDesc.bindings.reserve(mTextures.size());
        for (uint32_t index = 0; index < mTextures.size(); ++index)
        {
            const bool bLive = mTextures[index] != nullptr;
            nvrhi::ITexture* texture = bLive ? mTextures[index].Get() : mTextures[0].Get();
            const TextureView& view = bLive ? mTextureViews[index] : mTextureViews[0];
            setDesc.addItem(
                nvrhi::BindingSetItem::Texture_SRV(
                    0,
                    texture,
                    view.Format,
                    view.Subresources,
                    view.Dimension).setArrayElement(index));
        }
        nvrhi::BindingSetHandle replacement = mDevice->createBindingSet(setDesc, mLayout);
        if (!replacement)
            throw std::runtime_error("NVRHI failed to rebuild the bounded texture descriptor table");
        mBoundedBindingSet = std::move(replacement);
    }

    nvrhi::IBindingSet* BindlessTextureTable::GetBindingSet() const noexcept
    {
        return mAccess == ShaderDescriptorAccess::HeapDirectlyIndexed
            ? mTable.Get()
            : mBoundedBindingSet.Get();
    }

    BindlessSamplerTable::BindlessSamplerTable(
        nvrhi::IDevice* device,
        const BindlessSamplerTableDesc& desc)
        : mDevice(device)
        , mAllocator(desc.Capacity)
        , mAccess(desc.Access)
        , mSamplers(desc.Capacity)
    {
        if (!mDevice)
            throw std::invalid_argument("BindlessSamplerTable requires a valid NVRHI device");
        if (desc.Visibility == nvrhi::ShaderType::None)
            throw std::invalid_argument("BindlessSamplerTable shader visibility must not be None");

        if (mAccess == ShaderDescriptorAccess::HeapDirectlyIndexed)
        {
            nvrhi::BindlessLayoutDesc layoutDesc;
            layoutDesc.visibility = desc.Visibility;
            layoutDesc.firstSlot = 0;
            layoutDesc.maxCapacity = desc.Capacity;
            layoutDesc.layoutType = nvrhi::BindlessLayoutDesc::LayoutType::MutableSampler;
            mLayout = mDevice->createBindlessLayout(layoutDesc);
        }
        else
        {
            if (desc.Capacity > std::numeric_limits<uint16_t>::max())
                throw std::invalid_argument("Bounded sampler descriptor capacity exceeds the NVRHI layout limit");
            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.visibility = desc.Visibility;
            layoutDesc.setRegisterSpaceAndDescriptorSet(desc.RegisterSpace);
            layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0).setSize(desc.Capacity));
            mLayout = mDevice->createBindingLayout(layoutDesc);
        }
        if (!mLayout)
        {
            throw std::runtime_error(
                "NVRHI failed to create sampler descriptor layout '" + desc.DebugName + "'");
        }

        if (mAccess == ShaderDescriptorAccess::HeapDirectlyIndexed)
        {
            mTable = mDevice->createDescriptorTable(mLayout);
            if (!mTable)
            {
                throw std::runtime_error(
                    "NVRHI failed to create heap-direct sampler descriptor table '" +
                    desc.DebugName + "'");
            }

            mDevice->resizeDescriptorTable(mTable, desc.Capacity, false);
            if (mTable->getCapacity() != desc.Capacity)
            {
                throw std::runtime_error(
                    "NVRHI heap-direct sampler table capacity does not match the request for '" +
                    desc.DebugName + "' (requested=" + std::to_string(desc.Capacity) +
                    ", actual=" + std::to_string(mTable->getCapacity()) + ")");
            }
            mShaderDescriptorBase = mTable->getFirstDescriptorIndexInHeap();
        }
    }

    BindlessSamplerHandle BindlessSamplerTable::AllocateSampler(const nvrhi::SamplerHandle& sampler)
    {
        if (!sampler)
            throw std::invalid_argument("Cannot allocate a bindless descriptor for a null sampler");

        DescriptorAllocationHandle allocation;
        try
        {
            allocation = mAllocator.Allocate();
        }
        catch (const std::runtime_error& exception)
        {
            throw std::runtime_error(
                std::string(mAccess == ShaderDescriptorAccess::BoundedTable
                    ? "Baseline bounded material sampler table is full; reduce simultaneously resident samplers or add descriptor paging. "
                    : "Heap-direct material sampler table is full. ") +
                exception.what());
        }
        mSamplers[allocation.Index] = sampler;
        try
        {
            WriteSampler(allocation.Index, sampler);
        }
        catch (...)
        {
            mSamplers[allocation.Index] = nullptr;
            mAllocator.Release(allocation, 0);
            throw;
        }
        return { allocation.Index, allocation.Generation };
    }

    void BindlessSamplerTable::UpdateSampler(
        BindlessSamplerHandle handle,
        const nvrhi::SamplerHandle& sampler)
    {
        if (!IsValid(handle))
            throw std::out_of_range("Cannot update an invalid or stale bindless sampler handle");
        if (!sampler)
            throw std::invalid_argument("Cannot update a bindless descriptor with a null sampler");

        const nvrhi::SamplerHandle previousSampler = mSamplers[handle.Index];
        mSamplers[handle.Index] = sampler;
        try
        {
            WriteSampler(handle.Index, sampler);
        }
        catch (...)
        {
            mSamplers[handle.Index] = previousSampler;
            if (mAccess == ShaderDescriptorAccess::BoundedTable)
                RebuildBoundedBindingSet();
            throw;
        }
    }

    void BindlessSamplerTable::ReleaseSampler(
        BindlessSamplerHandle handle,
        uint64_t retireAfterSubmission)
    {
        if (!IsValid(handle))
            throw std::out_of_range("Cannot release an invalid or stale bindless sampler handle");

        const nvrhi::SamplerHandle retainedSampler = mSamplers[handle.Index];
        if (mAccess == ShaderDescriptorAccess::BoundedTable)
        {
            mSamplers[handle.Index] = nullptr;
            try
            {
                RebuildBoundedBindingSet();
            }
            catch (...)
            {
                mSamplers[handle.Index] = retainedSampler;
                RebuildBoundedBindingSet();
                throw;
            }
        }

        if (retireAfterSubmission == 0)
        {
            // D3D12 has no null sampler descriptor. The slot may be overwritten
            // immediately only because an immediate retirement promises no GPU use.
            if (mAccess == ShaderDescriptorAccess::HeapDirectlyIndexed)
                mSamplers[handle.Index] = nullptr;
        }
        else
        {
            mRetiredSamplers.push_back({ handle.Index, retireAfterSubmission, retainedSampler });
            if (mAccess == ShaderDescriptorAccess::HeapDirectlyIndexed)
                mSamplers[handle.Index] = nullptr;
        }
        mAllocator.Release({ handle.Index, handle.Generation }, retireAfterSubmission);
    }

    void BindlessSamplerTable::Collect(uint64_t completedSubmission)
    {
        std::erase_if(mRetiredSamplers, [completedSubmission](const RetainedSampler& sampler)
        {
            return sampler.Submission <= completedSubmission;
        });
        mAllocator.Collect(completedSubmission);
    }

    bool BindlessSamplerTable::IsValid(BindlessSamplerHandle handle) const noexcept
    {
        return mAllocator.IsValid({ handle.Index, handle.Generation });
    }

    uint32_t BindlessSamplerTable::GetShaderDescriptorIndex(BindlessSamplerHandle handle) const
    {
        if (!IsValid(handle))
            throw std::out_of_range("Invalid or stale bindless sampler handle");
        if (mAccess == ShaderDescriptorAccess::BoundedTable)
            return handle.Index;
        if (mShaderDescriptorBase > std::numeric_limits<uint32_t>::max() - handle.Index)
            throw std::overflow_error("Bindless sampler shader descriptor index overflow");
        return mShaderDescriptorBase + handle.Index;
    }

    void BindlessSamplerTable::WriteSampler(uint32_t index, const nvrhi::SamplerHandle& sampler)
    {
        if (mAccess == ShaderDescriptorAccess::BoundedTable)
        {
            RebuildBoundedBindingSet();
            return;
        }
        if (!mDevice->writeDescriptorTable(
                mTable,
                nvrhi::BindingSetItem::Sampler(index, sampler)))
        {
            throw std::runtime_error(
                "NVRHI failed to write bindless sampler descriptor index " + std::to_string(index));
        }
    }

    void BindlessSamplerTable::RebuildBoundedBindingSet()
    {
        if (mAccess != ShaderDescriptorAccess::BoundedTable || mSamplers.empty() || !mSamplers[0])
            return;

        nvrhi::BindingSetDesc setDesc;
        setDesc.bindings.reserve(mSamplers.size());
        for (uint32_t index = 0; index < mSamplers.size(); ++index)
        {
            nvrhi::ISampler* sampler = mSamplers[index]
                ? mSamplers[index].Get()
                : mSamplers[0].Get();
            setDesc.addItem(
                nvrhi::BindingSetItem::Sampler(0, sampler).setArrayElement(index));
        }
        nvrhi::BindingSetHandle replacement = mDevice->createBindingSet(setDesc, mLayout);
        if (!replacement)
            throw std::runtime_error("NVRHI failed to rebuild the bounded sampler descriptor table");
        mBoundedBindingSet = std::move(replacement);
    }

    nvrhi::IBindingSet* BindlessSamplerTable::GetBindingSet() const noexcept
    {
        return mAccess == ShaderDescriptorAccess::HeapDirectlyIndexed
            ? mTable.Get()
            : mBoundedBindingSet.Get();
    }

    BindlessDescriptorManager::BindlessDescriptorManager(
        nvrhi::IDevice* device,
        const BindlessDescriptorManagerDesc& desc)
        : mTextures(device, desc.Textures)
        , mSamplers(device, desc.Samplers)
    {
    }

    void BindlessDescriptorManager::Collect(uint64_t completedSubmission)
    {
        mTextures.Collect(completedSubmission);
        mSamplers.Collect(completedSubmission);
    }
}
