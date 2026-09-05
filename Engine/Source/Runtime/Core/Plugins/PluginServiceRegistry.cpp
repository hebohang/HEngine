#include "Hepch.h"

#include "Runtime/Core/Plugins/PluginServiceRegistry.h"

#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace HEngine
{
    namespace Detail
    {
        struct PluginServiceEntry
        {
            std::string Id;
            std::string Owner;
            std::type_index Type{ typeid(void) };
            Ref<void> Instance;
            uint64_t Token = 0;
        };

        struct PluginServiceRegistryState
        {
            mutable std::mutex Mutex;
            std::unordered_map<std::string, PluginServiceEntry> Entries;
            std::unordered_map<uint64_t, std::string> TokenToId;
            uint64_t NextToken = 1;
        };
    }

    namespace
    {
        [[nodiscard]] bool IsValidServiceId(std::string_view value) noexcept
        {
            if (value.empty() || value.size() > 128)
                return false;
            for (unsigned char character : value)
            {
                const bool bAllowed =
                    (character >= 'a' && character <= 'z') ||
                    (character >= 'A' && character <= 'Z') ||
                    (character >= '0' && character <= '9') ||
                    character == '.' || character == '_' || character == '-';
                if (!bAllowed)
                    return false;
            }
            return true;
        }

        void RevokeToken(
            const std::shared_ptr<Detail::PluginServiceRegistryState>& state,
            uint64_t token) noexcept
        {
            if (!state || token == 0)
                return;
            const std::scoped_lock lock(state->Mutex);
            const auto tokenEntry = state->TokenToId.find(token);
            if (tokenEntry == state->TokenToId.end())
                return;
            const auto serviceEntry = state->Entries.find(tokenEntry->second);
            if (serviceEntry != state->Entries.end() &&
                serviceEntry->second.Token == token)
            {
                state->Entries.erase(serviceEntry);
            }
            state->TokenToId.erase(tokenEntry);
        }

        [[nodiscard]] bool IsTokenActive(
            const std::shared_ptr<Detail::PluginServiceRegistryState>& state,
            uint64_t token) noexcept
        {
            if (!state || token == 0)
                return false;
            const std::scoped_lock lock(state->Mutex);
            return state->TokenToId.contains(token);
        }
    }

    PluginServiceHandle::PluginServiceHandle(
        std::weak_ptr<Detail::PluginServiceRegistryState> state,
        uint64_t token) noexcept
        : mState(std::move(state))
        , mToken(token)
    {
    }

    PluginServiceHandle::~PluginServiceHandle()
    {
        Revoke();
    }

    PluginServiceHandle::PluginServiceHandle(
        PluginServiceHandle&& other) noexcept
        : mState(std::move(other.mState))
        , mToken(other.mToken)
    {
        other.mToken = 0;
    }

    PluginServiceHandle& PluginServiceHandle::operator=(
        PluginServiceHandle&& other) noexcept
    {
        if (this == &other)
            return *this;
        Revoke();
        mState = std::move(other.mState);
        mToken = other.mToken;
        other.mToken = 0;
        return *this;
    }

    void PluginServiceHandle::Revoke() noexcept
    {
        RevokeToken(mState.lock(), mToken);
        mState.reset();
        mToken = 0;
    }

    bool PluginServiceHandle::IsActive() const noexcept
    {
        return IsTokenActive(mState.lock(), mToken);
    }

    PluginServiceRegistry::PluginServiceRegistry()
        : mState(std::make_shared<Detail::PluginServiceRegistryState>())
    {
    }

    PluginServiceRegistry::~PluginServiceRegistry()
    {
        Clear();
    }

    PluginServiceHandle PluginServiceRegistry::RegisterErased(
        std::string serviceId,
        std::string owner,
        std::type_index type,
        Ref<void> service)
    {
        if (!IsValidServiceId(serviceId))
            throw std::invalid_argument("Plugin service Id is invalid: " + serviceId);
        if (owner.empty())
            throw std::invalid_argument("Plugin service owner must not be empty");
        if (!service || type == std::type_index(typeid(void)))
            throw std::invalid_argument("Plugin service instance and type must be valid");

        const std::scoped_lock lock(mState->Mutex);
        if (mState->Entries.contains(serviceId))
            throw std::logic_error("Plugin service is already registered: " + serviceId);
        const uint64_t token = mState->NextToken++;
        mState->Entries.emplace(
            serviceId,
            Detail::PluginServiceEntry{
                .Id = serviceId,
                .Owner = std::move(owner),
                .Type = type,
                .Instance = std::move(service),
                .Token = token,
            });
        mState->TokenToId.emplace(token, serviceId);
        return PluginServiceHandle(mState, token);
    }

    PluginServiceView PluginServiceRegistry::Find(
        std::string_view serviceId) const
    {
        const std::scoped_lock lock(mState->Mutex);
        const auto entry = mState->Entries.find(std::string(serviceId));
        if (entry == mState->Entries.end())
            return {};
        return {
            .Id = entry->second.Id,
            .Owner = entry->second.Owner,
            .Type = entry->second.Type,
            .Instance = entry->second.Instance,
        };
    }

    bool PluginServiceRegistry::Contains(std::string_view serviceId) const
    {
        const std::scoped_lock lock(mState->Mutex);
        return mState->Entries.contains(std::string(serviceId));
    }

    size_t PluginServiceRegistry::Size() const
    {
        const std::scoped_lock lock(mState->Mutex);
        return mState->Entries.size();
    }

    size_t PluginServiceRegistry::RevokeOwner(std::string_view owner) noexcept
    {
        const std::scoped_lock lock(mState->Mutex);
        size_t removedCount = 0;
        for (auto entry = mState->Entries.begin();
            entry != mState->Entries.end();)
        {
            if (entry->second.Owner != owner)
            {
                ++entry;
                continue;
            }
            mState->TokenToId.erase(entry->second.Token);
            entry = mState->Entries.erase(entry);
            ++removedCount;
        }
        return removedCount;
    }

    void PluginServiceRegistry::Clear() noexcept
    {
        const std::scoped_lock lock(mState->Mutex);
        mState->Entries.clear();
        mState->TokenToId.clear();
    }
}
