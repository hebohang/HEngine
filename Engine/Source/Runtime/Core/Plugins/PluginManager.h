#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Core/Plugins/PluginApi.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
    enum class PluginState : uint8_t
    {
        Discovered,
        Registered,
        Loading,
        Loaded,
        Started,
        Stopping,
        Unloaded,
        Failed,
    };

    struct PluginStatus
    {
        PluginId Id;
        PluginState State = PluginState::Discovered;
        size_t RegistrationOrder = 0;
        uint64_t StartOrder = 0;
        std::string Diagnostic;
    };

    class PluginLifecycleError : public std::runtime_error
    {
    public:
        PluginLifecycleError(
            PluginId pluginId,
            std::string operation,
            std::string detail);

        [[nodiscard]] const PluginId& GetPluginId() const noexcept;
        [[nodiscard]] const std::string& GetOperation() const noexcept;

    private:
        PluginId mPluginId;
        std::string mOperation;
    };

    class PluginManager final
    {
    public:
        explicit PluginManager(PluginHostEnvironment environment);
        ~PluginManager();

        PluginManager(const PluginManager&) = delete;
        PluginManager& operator=(const PluginManager&) = delete;

        void Register(StaticPluginRegistration registration);
        bool Unregister(std::string_view pluginId);

        [[nodiscard]] bool Contains(std::string_view pluginId) const;
        [[nodiscard]] size_t Size() const noexcept;
        [[nodiscard]] size_t ActiveCount() const noexcept;
        [[nodiscard]] PluginStatus GetStatus(std::string_view pluginId) const;
        [[nodiscard]] std::vector<PluginStatus> EnumerateStatuses() const;
        [[nodiscard]] std::vector<PluginDescriptor> EnumerateDescriptors() const;

        void StartThroughPhase(PluginLoadPhase phase);
        void StopAll();

        [[nodiscard]] const PluginHostEnvironment& GetEnvironment() const noexcept;
        [[nodiscard]] PluginServiceRegistry& GetServices() noexcept;
        [[nodiscard]] const PluginServiceRegistry& GetServices() const noexcept;

    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
