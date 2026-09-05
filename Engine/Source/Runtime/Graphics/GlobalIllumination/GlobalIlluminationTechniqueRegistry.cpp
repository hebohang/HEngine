#include "Hepch.h"

#include "Runtime/Graphics/GlobalIllumination/GlobalIlluminationTechniqueRegistry.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] bool IsFeatureAvailable(
            const RenderFeatureStatus& feature) noexcept
        {
            return feature.bSupported && feature.bEnabled;
        }

        [[nodiscard]] bool HasRadianceOutput(
            GlobalIlluminationTechniqueOutput outputs) noexcept
        {
            constexpr GlobalIlluminationTechniqueOutput radianceOutputs =
                GlobalIlluminationTechniqueOutput::DiffuseIndirectRadiance |
                GlobalIlluminationTechniqueOutput::DirectDiffuseRadiance |
                GlobalIlluminationTechniqueOutput::SpecularIndirectRadiance;
            return (static_cast<uint32_t>(outputs) &
                    static_cast<uint32_t>(radianceOutputs)) != 0u;
        }

        void ValidateDescriptor(
            const GlobalIlluminationTechniqueDescriptor& descriptor)
        {
            if (descriptor.DisplayName.empty())
            {
                throw std::invalid_argument(
                    "Global illumination technique display name must not be empty.");
            }
            if (!descriptor.Factory)
            {
                throw std::invalid_argument(
                    "Global illumination technique factory must not be empty.");
            }
            if (HasRadianceOutput(descriptor.OutputSemantics.Outputs) &&
                (!descriptor.OutputSemantics.bSceneLinearHdr ||
                    !descriptor.OutputSemantics.bPreExposed ||
                    !descriptor.OutputSemantics.bPreTonemap))
            {
                throw std::invalid_argument(
                    "Global illumination radiance outputs must be scene-linear HDR, pre-exposed, and pre-tonemap.");
            }
            for (size_t index = 0;
                index < descriptor.SupportedDebugViews.size();
                ++index)
            {
                if (std::find(
                        descriptor.SupportedDebugViews.begin(),
                        descriptor.SupportedDebugViews.begin() + index,
                        descriptor.SupportedDebugViews[index]) !=
                    descriptor.SupportedDebugViews.begin() + index)
                {
                    throw std::invalid_argument(
                        "Global illumination technique debug views must be unique.");
                }
            }
        }
    }

    namespace Detail
    {
        struct GlobalIlluminationTechniqueRegistryEntry
        {
            GlobalIlluminationTechniqueDescriptor Descriptor;
            size_t ActiveLeaseCount = 0;
        };

        struct GlobalIlluminationTechniqueRegistryState
        {
            mutable std::mutex Mutex;
            std::vector<GlobalIlluminationTechniqueRegistryEntry> Entries;
        };

        [[nodiscard]] auto FindEntry(
            GlobalIlluminationTechniqueRegistryState& state,
            GlobalIlluminationTechniqueKind id)
        {
            return std::find_if(
                state.Entries.begin(),
                state.Entries.end(),
                [id](const GlobalIlluminationTechniqueRegistryEntry& entry)
                {
                    return entry.Descriptor.Id == id;
                });
        }

        [[nodiscard]] auto FindEntry(
            const GlobalIlluminationTechniqueRegistryState& state,
            GlobalIlluminationTechniqueKind id)
        {
            return std::find_if(
                state.Entries.cbegin(),
                state.Entries.cend(),
                [id](const GlobalIlluminationTechniqueRegistryEntry& entry)
                {
                    return entry.Descriptor.Id == id;
                });
        }

        void ReleaseLease(
            const std::shared_ptr<GlobalIlluminationTechniqueRegistryState>& state,
            GlobalIlluminationTechniqueKind id) noexcept
        {
            if (!state)
                return;

            const std::scoped_lock lock(state->Mutex);
            const auto entry = FindEntry(*state, id);
            if (entry != state->Entries.end() && entry->ActiveLeaseCount > 0)
                --entry->ActiveLeaseCount;
        }
    }

    bool GlobalIlluminationTechniqueDescriptor::AreCapabilitiesSatisfied(
        const RenderCapabilities& capabilities) const noexcept
    {
        const auto has = [this](
                             GlobalIlluminationTechniqueCapabilityRequirements
                                 requirement)
        {
            return HasGlobalIlluminationTechniqueCapabilityRequirement(
                CapabilityRequirements,
                requirement);
        };

        return (!has(GlobalIlluminationTechniqueCapabilityRequirements::GraphicsQueue) ||
                   capabilities.bGraphicsQueue) &&
            (!has(GlobalIlluminationTechniqueCapabilityRequirements::ComputeQueue) ||
                capabilities.bComputeQueue) &&
            (!has(GlobalIlluminationTechniqueCapabilityRequirements::
                       RayTracingAccelerationStructure) ||
                IsFeatureAvailable(capabilities.RayTracingAccelStruct)) &&
            (!has(GlobalIlluminationTechniqueCapabilityRequirements::
                       RayTracingPipeline) ||
                IsFeatureAvailable(capabilities.RayTracingPipeline)) &&
            (!has(GlobalIlluminationTechniqueCapabilityRequirements::RayQuery) ||
                IsFeatureAvailable(capabilities.RayQuery));
    }

    GlobalIlluminationTechniqueLease::GlobalIlluminationTechniqueLease(
        Scope<IGlobalIlluminationTechnique> technique,
        std::shared_ptr<Detail::GlobalIlluminationTechniqueRegistryState> state,
        GlobalIlluminationTechniqueKind id) noexcept
        : mTechnique(std::move(technique)),
          mState(std::move(state)),
          mId(id)
    {
    }

    GlobalIlluminationTechniqueLease::~GlobalIlluminationTechniqueLease()
    {
        Release();
    }

    GlobalIlluminationTechniqueLease::GlobalIlluminationTechniqueLease(
        GlobalIlluminationTechniqueLease&& other) noexcept
        : mTechnique(std::move(other.mTechnique)),
          mState(std::move(other.mState)),
          mId(other.mId)
    {
        other.mId = GlobalIlluminationTechniqueKind::Off;
    }

    GlobalIlluminationTechniqueLease&
        GlobalIlluminationTechniqueLease::operator=(
            GlobalIlluminationTechniqueLease&& other) noexcept
    {
        if (this == &other)
            return *this;

        Release();
        mTechnique = std::move(other.mTechnique);
        mState = std::move(other.mState);
        mId = other.mId;
        other.mId = GlobalIlluminationTechniqueKind::Off;
        return *this;
    }

    IGlobalIlluminationTechnique* GlobalIlluminationTechniqueLease::Get() const noexcept
    {
        return mTechnique.get();
    }

    IGlobalIlluminationTechnique*
        GlobalIlluminationTechniqueLease::operator->() const noexcept
    {
        return Get();
    }

    IGlobalIlluminationTechnique& GlobalIlluminationTechniqueLease::operator*() const
    {
        if (!mTechnique)
        {
            throw std::logic_error(
                "Cannot dereference an empty global illumination technique lease.");
        }
        return *mTechnique;
    }

    GlobalIlluminationTechniqueLease::operator bool() const noexcept
    {
        return mTechnique != nullptr;
    }

    GlobalIlluminationTechniqueKind
        GlobalIlluminationTechniqueLease::GetId() const noexcept
    {
        return mId;
    }

    void GlobalIlluminationTechniqueLease::Release() noexcept
    {
        if (!mState)
            return;

        mTechnique.reset();
        Detail::ReleaseLease(mState, mId);
        mState.reset();
        mId = GlobalIlluminationTechniqueKind::Off;
    }

    GlobalIlluminationTechniqueRegistry::GlobalIlluminationTechniqueRegistry()
        : mState(std::make_shared<
              Detail::GlobalIlluminationTechniqueRegistryState>())
    {
    }

    GlobalIlluminationTechniqueRegistry::~GlobalIlluminationTechniqueRegistry() =
        default;

    void GlobalIlluminationTechniqueRegistry::Register(
        GlobalIlluminationTechniqueDescriptor descriptor)
    {
        ValidateDescriptor(descriptor);
        const std::scoped_lock lock(mState->Mutex);
        if (Detail::FindEntry(*mState, descriptor.Id) != mState->Entries.end())
        {
            throw std::logic_error(
                "Global illumination technique is already registered.");
        }
        mState->Entries.push_back({ std::move(descriptor), 0 });
    }

    void GlobalIlluminationTechniqueRegistry::Unregister(
        GlobalIlluminationTechniqueKind id)
    {
        const std::scoped_lock lock(mState->Mutex);
        const auto entry = Detail::FindEntry(*mState, id);
        if (entry == mState->Entries.end())
        {
            throw std::out_of_range(
                "Cannot unregister an unknown global illumination technique.");
        }
        if (entry->ActiveLeaseCount != 0)
        {
            throw std::logic_error(
                "Cannot unregister a global illumination technique while it is active.");
        }
        mState->Entries.erase(entry);
    }

    bool GlobalIlluminationTechniqueRegistry::Contains(
        GlobalIlluminationTechniqueKind id) const
    {
        const std::scoped_lock lock(mState->Mutex);
        return Detail::FindEntry(*mState, id) != mState->Entries.end();
    }

    std::vector<GlobalIlluminationTechniqueDescriptor>
        GlobalIlluminationTechniqueRegistry::Enumerate() const
    {
        const std::scoped_lock lock(mState->Mutex);
        std::vector<GlobalIlluminationTechniqueDescriptor> descriptors;
        descriptors.reserve(mState->Entries.size());
        for (const Detail::GlobalIlluminationTechniqueRegistryEntry& entry :
            mState->Entries)
        {
            descriptors.push_back(entry.Descriptor);
        }
        return descriptors;
    }

    GlobalIlluminationTechniqueLease GlobalIlluminationTechniqueRegistry::Create(
        GlobalIlluminationTechniqueKind id)
    {
        GlobalIlluminationTechniqueFactory factory;
        {
            const std::scoped_lock lock(mState->Mutex);
            const auto entry = Detail::FindEntry(*mState, id);
            if (entry == mState->Entries.end())
            {
                throw std::out_of_range(
                    "Cannot create an unknown global illumination technique.");
            }
            factory = entry->Descriptor.Factory;
            ++entry->ActiveLeaseCount;
        }

        Scope<IGlobalIlluminationTechnique> technique;
        try
        {
            technique = factory();
        }
        catch (...)
        {
            Detail::ReleaseLease(mState, id);
            throw;
        }

        if (!technique)
        {
            Detail::ReleaseLease(mState, id);
            throw std::runtime_error(
                "Global illumination technique factory returned null.");
        }
        return GlobalIlluminationTechniqueLease(
            std::move(technique),
            mState,
            id);
    }

    size_t GlobalIlluminationTechniqueRegistry::GetActiveLeaseCount(
        GlobalIlluminationTechniqueKind id) const
    {
        const std::scoped_lock lock(mState->Mutex);
        const auto entry = Detail::FindEntry(*mState, id);
        if (entry == mState->Entries.end())
        {
            throw std::out_of_range(
                "Cannot query an unknown global illumination technique.");
        }
        return entry->ActiveLeaseCount;
    }
}
