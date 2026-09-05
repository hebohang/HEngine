#include "Hepch.h"

#include "Runtime/Graphics/Renderer/ViewportRendererExtension.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] bool IsStableId(std::string_view id) noexcept
        {
            return !id.empty() && std::ranges::all_of(
                id,
                [](unsigned char value)
                {
                    return std::isalnum(value) != 0 || value == '.' ||
                        value == '_' || value == '-';
                });
        }

        void ValidateDescriptor(
            const OfflineViewportRendererProviderDescriptor& descriptor)
        {
            if (!IsStableId(descriptor.Id))
                throw std::invalid_argument(
                    "Offline viewport renderer provider ID is invalid");
            if (descriptor.DisplayName.empty())
                throw std::invalid_argument(
                    "Offline viewport renderer display name is empty");
            if (descriptor.OwnerPluginId.empty())
                throw std::invalid_argument(
                    "Offline viewport renderer owner plugin ID is empty");
            if (!descriptor.Factory)
                throw std::invalid_argument(
                    "Offline viewport renderer factory is empty");
        }

        [[nodiscard]] bool IsFeatureAvailable(
            const RenderFeatureStatus& feature) noexcept
        {
            return feature.bSupported && feature.bEnabled;
        }

        [[nodiscard]] bool CapabilitiesSatisfied(
            GlobalIlluminationTechniqueCapabilityRequirements requirements,
            const RenderCapabilities& capabilities) noexcept
        {
            const auto has = [requirements](
                                 GlobalIlluminationTechniqueCapabilityRequirements
                                     requirement)
            {
                return HasGlobalIlluminationTechniqueCapabilityRequirement(
                    requirements,
                    requirement);
            };
            return (!has(GlobalIlluminationTechniqueCapabilityRequirements::
                         GraphicsQueue) || capabilities.bGraphicsQueue) &&
                (!has(GlobalIlluminationTechniqueCapabilityRequirements::
                         ComputeQueue) || capabilities.bComputeQueue) &&
                (!has(GlobalIlluminationTechniqueCapabilityRequirements::
                         RayTracingAccelerationStructure) ||
                    IsFeatureAvailable(capabilities.RayTracingAccelStruct)) &&
                (!has(GlobalIlluminationTechniqueCapabilityRequirements::
                         RayTracingPipeline) ||
                    IsFeatureAvailable(capabilities.RayTracingPipeline)) &&
                (!has(GlobalIlluminationTechniqueCapabilityRequirements::
                         RayQuery) || IsFeatureAvailable(capabilities.RayQuery));
        }
    }

    namespace Detail
    {
        struct OfflineViewportRendererRegistryEntry
        {
            OfflineViewportRendererProviderDescriptor Descriptor;
            size_t ActiveSessionCount = 0;
            bool bDraining = false;
        };

        struct OfflineViewportRendererRegistryState
        {
            mutable std::mutex Mutex;
            std::vector<OfflineViewportRendererRegistryEntry> Entries;
        };

        [[nodiscard]] auto FindEntry(
            OfflineViewportRendererRegistryState& state,
            std::string_view id)
        {
            return std::ranges::find_if(
                state.Entries,
                [id](const OfflineViewportRendererRegistryEntry& entry)
                {
                    return entry.Descriptor.Id == id;
                });
        }

        [[nodiscard]] auto FindEntry(
            const OfflineViewportRendererRegistryState& state,
            std::string_view id)
        {
            return std::ranges::find_if(
                state.Entries,
                [id](const OfflineViewportRendererRegistryEntry& entry)
                {
                    return entry.Descriptor.Id == id;
                });
        }

        void ReleaseSession(
            const std::shared_ptr<OfflineViewportRendererRegistryState>& state,
            std::string_view id) noexcept
        {
            if (!state)
                return;
            const std::scoped_lock lock(state->Mutex);
            const auto entry = FindEntry(*state, id);
            if (entry == state->Entries.end() || entry->ActiveSessionCount == 0)
                return;
            --entry->ActiveSessionCount;
            if (entry->bDraining && entry->ActiveSessionCount == 0)
                state->Entries.erase(entry);
        }

        void BeginDrain(
            const std::shared_ptr<OfflineViewportRendererRegistryState>& state,
            std::string_view id) noexcept
        {
            if (!state)
                return;
            const std::scoped_lock lock(state->Mutex);
            const auto entry = FindEntry(*state, id);
            if (entry == state->Entries.end())
                return;
            entry->bDraining = true;
            if (entry->ActiveSessionCount == 0)
                state->Entries.erase(entry);
        }
    }

    OfflineViewportRendererProviderRegistration::
        OfflineViewportRendererProviderRegistration(
            std::shared_ptr<Detail::OfflineViewportRendererRegistryState> state,
            std::string id) noexcept
        : mState(std::move(state)), mId(std::move(id))
    {
    }

    OfflineViewportRendererProviderRegistration::~
        OfflineViewportRendererProviderRegistration()
    {
        Release();
    }

    OfflineViewportRendererProviderRegistration::
        OfflineViewportRendererProviderRegistration(
            OfflineViewportRendererProviderRegistration&& other) noexcept
        : mState(std::move(other.mState)), mId(std::move(other.mId))
    {
    }

    OfflineViewportRendererProviderRegistration&
        OfflineViewportRendererProviderRegistration::operator=(
            OfflineViewportRendererProviderRegistration&& other) noexcept
    {
        if (this == &other)
            return *this;
        Release();
        mState = std::move(other.mState);
        mId = std::move(other.mId);
        return *this;
    }

    OfflineViewportRendererProviderRegistration::operator bool() const noexcept
    {
        if (!mState)
            return false;
        const std::scoped_lock lock(mState->Mutex);
        const auto entry = Detail::FindEntry(*mState, mId);
        return entry != mState->Entries.end() && !entry->bDraining;
    }

    void OfflineViewportRendererProviderRegistration::Release() noexcept
    {
        if (!mState)
            return;
        Detail::BeginDrain(mState, mId);
        mState.reset();
        mId.clear();
    }

    OfflineViewportRendererSessionLease::OfflineViewportRendererSessionLease(
        Scope<IOfflineViewportRendererSession> session,
        std::shared_ptr<Detail::OfflineViewportRendererRegistryState> state,
        std::string id) noexcept
        : mSession(std::move(session)),
          mState(std::move(state)),
          mId(std::move(id))
    {
    }

    OfflineViewportRendererSessionLease::~OfflineViewportRendererSessionLease()
    {
        Release();
    }

    OfflineViewportRendererSessionLease::OfflineViewportRendererSessionLease(
        OfflineViewportRendererSessionLease&& other) noexcept
        : mSession(std::move(other.mSession)),
          mState(std::move(other.mState)),
          mId(std::move(other.mId))
    {
    }

    OfflineViewportRendererSessionLease&
        OfflineViewportRendererSessionLease::operator=(
            OfflineViewportRendererSessionLease&& other) noexcept
    {
        if (this == &other)
            return *this;
        Release();
        mSession = std::move(other.mSession);
        mState = std::move(other.mState);
        mId = std::move(other.mId);
        return *this;
    }

    IOfflineViewportRendererSession*
        OfflineViewportRendererSessionLease::Get() const noexcept
    {
        return mSession.get();
    }

    IOfflineViewportRendererSession*
        OfflineViewportRendererSessionLease::operator->() const noexcept
    {
        return Get();
    }

    OfflineViewportRendererSessionLease::operator bool() const noexcept
    {
        return mSession != nullptr;
    }

    void OfflineViewportRendererSessionLease::Release() noexcept
    {
        if (!mState)
            return;
        mSession.reset();
        Detail::ReleaseSession(mState, mId);
        mState.reset();
        mId.clear();
    }

    OfflineViewportRendererProviderRegistry&
        OfflineViewportRendererProviderRegistry::Get()
    {
        static OfflineViewportRendererProviderRegistry registry;
        return registry;
    }

    OfflineViewportRendererProviderRegistry::
        OfflineViewportRendererProviderRegistry()
        : mState(std::make_shared<
              Detail::OfflineViewportRendererRegistryState>())
    {
    }

    OfflineViewportRendererProviderRegistration
        OfflineViewportRendererProviderRegistry::Register(
            OfflineViewportRendererProviderDescriptor descriptor)
    {
        ValidateDescriptor(descriptor);
        const std::scoped_lock lock(mState->Mutex);
        if (Detail::FindEntry(*mState, descriptor.Id) != mState->Entries.end())
            throw std::logic_error(
                "Offline viewport renderer provider is already registered");
        const std::string id = descriptor.Id;
        mState->Entries.push_back({ std::move(descriptor), 0, false });
        return OfflineViewportRendererProviderRegistration(mState, id);
    }

    void OfflineViewportRendererProviderRegistry::Unregister(
        std::string_view id)
    {
        const std::scoped_lock lock(mState->Mutex);
        const auto entry = Detail::FindEntry(*mState, id);
        if (entry == mState->Entries.end())
            return;
        entry->bDraining = true;
        if (entry->ActiveSessionCount == 0)
            mState->Entries.erase(entry);
    }

    bool OfflineViewportRendererProviderRegistry::Contains(
        std::string_view id) const
    {
        const std::scoped_lock lock(mState->Mutex);
        const auto entry = Detail::FindEntry(*mState, id);
        return entry != mState->Entries.end() && !entry->bDraining;
    }

    std::vector<OfflineViewportRendererProviderDescriptor>
        OfflineViewportRendererProviderRegistry::Enumerate() const
    {
        const std::scoped_lock lock(mState->Mutex);
        std::vector<OfflineViewportRendererProviderDescriptor> descriptors;
        descriptors.reserve(mState->Entries.size());
        for (const auto& entry : mState->Entries)
        {
            if (entry.bDraining)
                continue;
            OfflineViewportRendererProviderDescriptor descriptor =
                entry.Descriptor;
            descriptor.QuerySupport = {};
            descriptor.Factory = {};
            descriptors.push_back(std::move(descriptor));
        }
        std::ranges::stable_sort(
            descriptors,
            [](const auto& left, const auto& right)
            {
                return left.SortOrder < right.SortOrder;
            });
        return descriptors;
    }

    OfflineViewportRendererSupport
        OfflineViewportRendererProviderRegistry::QuerySupport(
            std::string_view id,
            const RenderCapabilities& capabilities) const
    {
        OfflineViewportRendererProviderDescriptor descriptor;
        {
            const std::scoped_lock lock(mState->Mutex);
            const auto entry = Detail::FindEntry(*mState, id);
            if (entry == mState->Entries.end() || entry->bDraining)
                return { false, "The viewport renderer provider is not registered." };
            descriptor = entry->Descriptor;
        }
        if (!CapabilitiesSatisfied(
                descriptor.CapabilityRequirements,
                capabilities))
        {
            return {
                false,
                "Required renderer capabilities are unavailable.",
            };
        }
        if (descriptor.QuerySupport)
            return descriptor.QuerySupport(
                capabilities,
                descriptor.ShaderArtifactCacheRoot);
        return { true, "The viewport renderer provider is available." };
    }

    OfflineViewportRendererSessionLease
        OfflineViewportRendererProviderRegistry::Create(
            std::string_view id,
            const OfflineViewportRendererCreateContext& context)
    {
        OfflineViewportRendererFactory factory;
        std::string ownedId;
        {
            const std::scoped_lock lock(mState->Mutex);
            const auto entry = Detail::FindEntry(*mState, id);
            if (entry == mState->Entries.end() || entry->bDraining)
                throw std::out_of_range(
                    "Offline viewport renderer provider is not registered");
            factory = entry->Descriptor.Factory;
            ownedId = entry->Descriptor.Id;
            ++entry->ActiveSessionCount;
        }

        Scope<IOfflineViewportRendererSession> session;
        try
        {
            session = factory(context);
        }
        catch (...)
        {
            Detail::ReleaseSession(mState, ownedId);
            throw;
        }
        if (!session)
        {
            Detail::ReleaseSession(mState, ownedId);
            throw std::runtime_error(
                "Offline viewport renderer factory returned null");
        }
        return OfflineViewportRendererSessionLease(
            std::move(session),
            mState,
            std::move(ownedId));
    }

    size_t OfflineViewportRendererProviderRegistry::GetActiveSessionCount(
        std::string_view id) const
    {
        const std::scoped_lock lock(mState->Mutex);
        const auto entry = Detail::FindEntry(*mState, id);
        if (entry == mState->Entries.end())
            throw std::out_of_range(
                "Offline viewport renderer provider is not registered");
        return entry->ActiveSessionCount;
    }
}
