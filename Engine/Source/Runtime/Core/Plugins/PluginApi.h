#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Core/Plugins/PluginDescriptor.h"
#include "Runtime/Core/Plugins/PluginServiceRegistry.h"

#include <type_traits>
#include <typeindex>
#include <utility>

namespace HEngine
{
    class PluginContext final
    {
    public:
        PluginContext(
            const PluginDescriptor& descriptor,
            const PluginHostEnvironment& environment,
            PluginServiceRegistry& services) noexcept
            : mDescriptor(descriptor)
            , mEnvironment(environment)
            , mServices(services)
        {
        }

        [[nodiscard]] const PluginDescriptor& GetDescriptor() const noexcept
        {
            return mDescriptor;
        }

        [[nodiscard]] const PluginHostEnvironment& GetEnvironment() const noexcept
        {
            return mEnvironment;
        }

        [[nodiscard]] PluginServiceRegistry& GetServices() const noexcept
        {
            return mServices;
        }

        [[nodiscard]] PluginServiceHandle RegisterServiceErased(
            std::string serviceId,
            std::type_index type,
            Ref<void> service)
        {
            return mServices.RegisterErased(
                std::move(serviceId),
                mDescriptor.Id,
                type,
                std::move(service));
        }

        template<typename Service>
        [[nodiscard]] PluginServiceHandle RegisterService(
            std::string serviceId,
            Ref<Service> service)
        {
            static_assert(!std::is_void_v<Service>);
            return mServices.Register<Service>(
                std::move(serviceId),
                mDescriptor.Id,
                std::move(service));
        }

    private:
        const PluginDescriptor& mDescriptor;
        const PluginHostEnvironment& mEnvironment;
        PluginServiceRegistry& mServices;
    };

    class IPluginModule
    {
    public:
        virtual ~IPluginModule() = default;

        virtual void Start(PluginContext& context) = 0;
        virtual void Stop(PluginContext& context) = 0;
    };

    using PluginModuleFactory = Scope<IPluginModule>(*)();

    struct StaticPluginRegistration
    {
        PluginDescriptor Descriptor;
        PluginModuleFactory Factory = nullptr;
    };
}
