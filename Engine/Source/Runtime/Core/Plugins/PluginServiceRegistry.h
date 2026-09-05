#pragma once

#include "Runtime/Core/Base/Base.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <utility>

namespace HEngine
{
    namespace Detail
    {
        struct PluginServiceRegistryState;
    }

    class PluginServiceHandle final
    {
    public:
        PluginServiceHandle() noexcept = default;
        ~PluginServiceHandle();

        PluginServiceHandle(const PluginServiceHandle&) = delete;
        PluginServiceHandle& operator=(const PluginServiceHandle&) = delete;
        PluginServiceHandle(PluginServiceHandle&& other) noexcept;
        PluginServiceHandle& operator=(PluginServiceHandle&& other) noexcept;

        void Revoke() noexcept;
        [[nodiscard]] bool IsActive() const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return IsActive();
        }

    private:
        friend class PluginServiceRegistry;

        PluginServiceHandle(
            std::weak_ptr<Detail::PluginServiceRegistryState> state,
            uint64_t token) noexcept;

        std::weak_ptr<Detail::PluginServiceRegistryState> mState;
        uint64_t mToken = 0;
    };

    struct PluginServiceView
    {
        std::string Id;
        std::string Owner;
        std::type_index Type{ typeid(void) };
        Ref<void> Instance;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return Instance != nullptr;
        }

        template<typename Service>
        [[nodiscard]] Ref<Service> As() const noexcept
        {
            static_assert(!std::is_void_v<Service>);
            if (Type != std::type_index(typeid(Service)) || !Instance)
                return nullptr;
            return std::static_pointer_cast<Service>(Instance);
        }
    };

    class PluginServiceRegistry final
    {
    public:
        PluginServiceRegistry();
        ~PluginServiceRegistry();

        PluginServiceRegistry(const PluginServiceRegistry&) = delete;
        PluginServiceRegistry& operator=(const PluginServiceRegistry&) = delete;

        [[nodiscard]] PluginServiceHandle RegisterErased(
            std::string serviceId,
            std::string owner,
            std::type_index type,
            Ref<void> service);

        template<typename Service>
        [[nodiscard]] PluginServiceHandle Register(
            std::string serviceId,
            std::string owner,
            Ref<Service> service)
        {
            static_assert(!std::is_void_v<Service>);
            return RegisterErased(
                std::move(serviceId),
                std::move(owner),
                std::type_index(typeid(Service)),
                std::move(service));
        }

        [[nodiscard]] PluginServiceView Find(std::string_view serviceId) const;

        template<typename Service>
        [[nodiscard]] Ref<Service> TryGet(std::string_view serviceId) const
        {
            return Find(serviceId).template As<Service>();
        }

        [[nodiscard]] bool Contains(std::string_view serviceId) const;
        [[nodiscard]] size_t Size() const;
        size_t RevokeOwner(std::string_view owner) noexcept;
        void Clear() noexcept;

    private:
        std::shared_ptr<Detail::PluginServiceRegistryState> mState;
    };
}
