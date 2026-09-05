#include "Hepch.h"

#include "Runtime/Core/Plugins/PluginManager.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] std::string DescribeException(
            const std::exception_ptr& exception)
        {
            if (!exception)
                return "Unknown failure";
            try
            {
                std::rethrow_exception(exception);
            }
            catch (const std::exception& error)
            {
                return error.what();
            }
            catch (...)
            {
                return "Non-standard exception";
            }
        }

        [[nodiscard]] std::string BuildLifecycleMessage(
            std::string_view pluginId,
            std::string_view operation,
            std::string_view detail)
        {
            return "Plugin '" + std::string(pluginId) + "' " +
                std::string(operation) + " failed: " + std::string(detail);
        }

        [[nodiscard]] bool IsActiveState(PluginState state) noexcept
        {
            return state == PluginState::Loading ||
                state == PluginState::Loaded ||
                state == PluginState::Started ||
                state == PluginState::Stopping;
        }
    }

    PluginLifecycleError::PluginLifecycleError(
        PluginId pluginId,
        std::string operation,
        std::string detail)
        : std::runtime_error(BuildLifecycleMessage(pluginId, operation, detail))
        , mPluginId(std::move(pluginId))
        , mOperation(std::move(operation))
    {
    }

    const PluginId& PluginLifecycleError::GetPluginId() const noexcept
    {
        return mPluginId;
    }

    const std::string& PluginLifecycleError::GetOperation() const noexcept
    {
        return mOperation;
    }

    struct PluginManager::Impl
    {
        struct Entry
        {
            PluginDescriptor Descriptor;
            PluginModuleFactory Factory = nullptr;
            Scope<IPluginModule> Module;
            PluginState State = PluginState::Discovered;
            uint64_t StartOrder = 0;
            std::string Diagnostic;
        };

        explicit Impl(PluginHostEnvironment environment)
            : Environment(std::move(environment))
        {
        }

        [[nodiscard]] Entry* Find(std::string_view pluginId)
        {
            const auto index = Indices.find(std::string(pluginId));
            return index == Indices.end() ? nullptr : &Entries[index->second];
        }

        [[nodiscard]] const Entry* Find(std::string_view pluginId) const
        {
            const auto index = Indices.find(std::string(pluginId));
            return index == Indices.end() ? nullptr : &Entries[index->second];
        }

        void RebuildIndices()
        {
            Indices.clear();
            for (size_t index = 0; index < Entries.size(); ++index)
                Indices.emplace(Entries[index].Descriptor.Id, index);
        }

        [[nodiscard]] PluginStatus MakeStatus(size_t index) const
        {
            const Entry& entry = Entries[index];
            return {
                .Id = entry.Descriptor.Id,
                .State = entry.State,
                .RegistrationOrder = index,
                .StartOrder = entry.StartOrder,
                .Diagnostic = entry.Diagnostic,
            };
        }

        [[nodiscard]] std::vector<size_t> BuildStartPlan(
            PluginLoadPhase phase)
        {
            const uint8_t maximumPhase = static_cast<uint8_t>(phase);
            if (maximumPhase > static_cast<uint8_t>(
                    PluginLoadPhase::OnDemand))
            {
                throw std::invalid_argument("Plugin load phase is invalid");
            }

            std::vector<bool> candidates(Entries.size(), false);
            size_t candidateCount = 0;
            for (size_t index = 0; index < Entries.size(); ++index)
            {
                if (static_cast<uint8_t>(Entries[index].Descriptor.LoadPhase) <=
                    maximumPhase)
                {
                    candidates[index] = true;
                    ++candidateCount;
                }
            }

            std::vector<size_t> indegrees(Entries.size(), 0);
            for (size_t index = 0; index < Entries.size(); ++index)
            {
                if (!candidates[index])
                    continue;
                Entry& entry = Entries[index];
                const auto addDependency = [this, &entry, &candidates,
                                               &indegrees, index](
                                               const PluginDependency& requirement,
                                               bool bOptional)
                {
                    const auto dependencyIndex = Indices.find(requirement.Id);
                    if (dependencyIndex == Indices.end())
                    {
                        if (bOptional)
                            return;
                        entry.Diagnostic =
                            "Missing dependency: " + requirement.Id;
                        throw PluginLifecycleError(
                            entry.Descriptor.Id,
                            "dependency resolution",
                            entry.Diagnostic);
                    }
                    const Entry& dependency = Entries[dependencyIndex->second];
                    if (!IsPluginVersionInRange(
                            dependency.Descriptor.Version,
                            requirement.VersionRange))
                    {
                        entry.Diagnostic =
                            "Dependency version is outside the required range: " +
                            requirement.Id;
                        throw PluginLifecycleError(
                            entry.Descriptor.Id,
                            "dependency resolution",
                            entry.Diagnostic);
                    }
                    if (!IsPluginApiCompatible(
                            dependency.Descriptor.ApiVersion,
                            requirement.MinimumApiVersion))
                    {
                        entry.Diagnostic =
                            "Dependency API version is incompatible: " +
                            requirement.Id;
                        throw PluginLifecycleError(
                            entry.Descriptor.Id,
                            "dependency resolution",
                            entry.Diagnostic);
                    }
                    if (static_cast<uint8_t>(dependency.Descriptor.LoadPhase) >
                        static_cast<uint8_t>(entry.Descriptor.LoadPhase))
                    {
                        entry.Diagnostic =
                            "Dependency belongs to a later load phase: " +
                            requirement.Id;
                        throw PluginLifecycleError(
                            entry.Descriptor.Id,
                            "dependency resolution",
                            entry.Diagnostic);
                    }
                    if (!candidates[dependencyIndex->second])
                    {
                        entry.Diagnostic =
                            "Dependency is not available in the requested phase: " +
                            requirement.Id;
                        throw PluginLifecycleError(
                            entry.Descriptor.Id,
                            "dependency resolution",
                            entry.Diagnostic);
                    }
                    ++indegrees[index];
                };
                for (const PluginDependency& dependency :
                    entry.Descriptor.Dependencies)
                {
                    addDependency(dependency, false);
                }
                for (const PluginDependency& dependency :
                    entry.Descriptor.OptionalDependencies)
                {
                    addDependency(dependency, true);
                }
            }

            std::vector<bool> emitted(Entries.size(), false);
            std::vector<size_t> plan;
            plan.reserve(candidateCount);
            while (plan.size() < candidateCount)
            {
                size_t selected = std::numeric_limits<size_t>::max();
                for (size_t index = 0; index < Entries.size(); ++index)
                {
                    if (candidates[index] && !emitted[index] &&
                        indegrees[index] == 0)
                    {
                        selected = index;
                        break;
                    }
                }
                if (selected == std::numeric_limits<size_t>::max())
                {
                    for (size_t index = 0; index < Entries.size(); ++index)
                    {
                        if (candidates[index] && !emitted[index])
                        {
                            Entries[index].Diagnostic =
                                "Dependency cycle detected";
                            throw PluginLifecycleError(
                                Entries[index].Descriptor.Id,
                                "dependency resolution",
                                Entries[index].Diagnostic);
                        }
                    }
                }

                emitted[selected] = true;
                plan.push_back(selected);
                const PluginId& selectedId = Entries[selected].Descriptor.Id;
                for (size_t index = 0; index < Entries.size(); ++index)
                {
                    if (!candidates[index] || emitted[index])
                        continue;
                    const auto hasDependency = [&selectedId](
                                                   const auto& dependencies)
                    {
                        return std::any_of(
                            dependencies.begin(),
                            dependencies.end(),
                            [&selectedId](const PluginDependency& dependency)
                            {
                                return dependency.Id == selectedId;
                            });
                    };
                    if (hasDependency(Entries[index].Descriptor.Dependencies) ||
                        hasDependency(
                            Entries[index].Descriptor.OptionalDependencies))
                    {
                        --indegrees[index];
                    }
                }
            }
            return plan;
        }

        void RollBackCurrentStart(
            Entry& entry,
            const std::exception_ptr& startFailure) noexcept
        {
            std::string diagnostic = DescribeException(startFailure);
            if (entry.Module)
            {
                entry.State = PluginState::Stopping;
                try
                {
                    PluginContext context(entry.Descriptor, Environment, Services);
                    entry.Module->Stop(context);
                }
                catch (...)
                {
                    diagnostic += "; rollback Stop failed: " +
                        DescribeException(std::current_exception());
                }
            }
            Services.RevokeOwner(entry.Descriptor.Id);
            entry.Module.reset();
            entry.State = PluginState::Failed;
            entry.StartOrder = 0;
            entry.Diagnostic = std::move(diagnostic);
        }

        void RollBackStarted(std::vector<size_t>& startedThisCall) noexcept
        {
            for (auto index = startedThisCall.rbegin();
                index != startedThisCall.rend();
                ++index)
            {
                Entry& entry = Entries[*index];
                std::string diagnostic = "Rolled back after a dependent plugin failed";
                entry.State = PluginState::Stopping;
                try
                {
                    PluginContext context(entry.Descriptor, Environment, Services);
                    entry.Module->Stop(context);
                }
                catch (...)
                {
                    diagnostic += "; Stop failed: " +
                        DescribeException(std::current_exception());
                }
                Services.RevokeOwner(entry.Descriptor.Id);
                entry.Module.reset();
                entry.State = PluginState::Unloaded;
                entry.StartOrder = 0;
                entry.Diagnostic = std::move(diagnostic);
                if (!ActiveOrder.empty() && ActiveOrder.back() == *index)
                    ActiveOrder.pop_back();
                else
                {
                    std::erase(ActiveOrder, *index);
                }
            }
            startedThisCall.clear();
        }

        PluginHostEnvironment Environment;
        PluginServiceRegistry Services;
        std::vector<Entry> Entries;
        std::unordered_map<std::string, size_t> Indices;
        std::vector<size_t> ActiveOrder;
        uint64_t NextStartOrder = 1;
    };

    PluginManager::PluginManager(PluginHostEnvironment environment)
        : mImpl(CreateScope<Impl>(std::move(environment)))
    {
        const uint32_t host = static_cast<uint32_t>(mImpl->Environment.Host);
        constexpr uint32_t ValidHosts =
            static_cast<uint32_t>(PluginHost::Editor) |
            static_cast<uint32_t>(PluginHost::DevGame) |
            static_cast<uint32_t>(PluginHost::Game) |
            static_cast<uint32_t>(PluginHost::Tests) |
            static_cast<uint32_t>(PluginHost::Tools) |
            static_cast<uint32_t>(PluginHost::RenderSmoke);
        if (host == 0u || (host & (host - 1u)) != 0u ||
            (host & ~ValidHosts) != 0u ||
            mImpl->Environment.ApiVersion.Major == 0)
        {
            throw std::invalid_argument(
                "PluginManager requires one host and a valid API version");
        }
    }

    PluginManager::~PluginManager()
    {
        try
        {
            StopAll();
        }
        catch (...)
        {
        }
    }

    void PluginManager::Register(StaticPluginRegistration registration)
    {
        const PluginValidationResult validation = ValidatePluginDescriptor(
            registration.Descriptor,
            mImpl->Environment);
        if (!validation)
            throw std::invalid_argument(validation.Message);
        if (!registration.Factory)
        {
            throw std::invalid_argument(
                "Static plugin registration requires a factory anchor: " +
                registration.Descriptor.Id);
        }
        if (mImpl->Indices.contains(registration.Descriptor.Id))
        {
            throw std::logic_error(
                "Plugin is already registered: " + registration.Descriptor.Id);
        }

        const PluginId pluginId = registration.Descriptor.Id;
        const size_t index = mImpl->Entries.size();
        mImpl->Entries.push_back(Impl::Entry{
            .Descriptor = std::move(registration.Descriptor),
            .Factory = registration.Factory,
        });
        try
        {
            mImpl->Indices.emplace(pluginId, index);
            mImpl->Entries[index].State = PluginState::Registered;
        }
        catch (...)
        {
            mImpl->Entries.pop_back();
            throw;
        }
    }

    bool PluginManager::Unregister(std::string_view pluginId)
    {
        const auto index = mImpl->Indices.find(std::string(pluginId));
        if (index == mImpl->Indices.end())
            return false;
        Impl::Entry& entry = mImpl->Entries[index->second];
        if (IsActiveState(entry.State))
        {
            throw std::logic_error(
                "Cannot unregister an active plugin: " + entry.Descriptor.Id);
        }

        mImpl->Services.RevokeOwner(entry.Descriptor.Id);
        entry.Module.reset();
        const size_t removedIndex = index->second;
        mImpl->Entries.erase(mImpl->Entries.begin() +
            static_cast<std::ptrdiff_t>(removedIndex));
        for (size_t& activeIndex : mImpl->ActiveOrder)
        {
            if (activeIndex > removedIndex)
                --activeIndex;
        }
        mImpl->RebuildIndices();
        return true;
    }

    bool PluginManager::Contains(std::string_view pluginId) const
    {
        return mImpl->Indices.contains(std::string(pluginId));
    }

    size_t PluginManager::Size() const noexcept
    {
        return mImpl->Entries.size();
    }

    size_t PluginManager::ActiveCount() const noexcept
    {
        return mImpl->ActiveOrder.size();
    }

    PluginStatus PluginManager::GetStatus(std::string_view pluginId) const
    {
        const auto index = mImpl->Indices.find(std::string(pluginId));
        if (index == mImpl->Indices.end())
            throw std::out_of_range("Unknown plugin: " + std::string(pluginId));
        return mImpl->MakeStatus(index->second);
    }

    std::vector<PluginStatus> PluginManager::EnumerateStatuses() const
    {
        std::vector<PluginStatus> statuses;
        statuses.reserve(mImpl->Entries.size());
        for (size_t index = 0; index < mImpl->Entries.size(); ++index)
            statuses.push_back(mImpl->MakeStatus(index));
        return statuses;
    }

    std::vector<PluginDescriptor> PluginManager::EnumerateDescriptors() const
    {
        std::vector<PluginDescriptor> descriptors;
        descriptors.reserve(mImpl->Entries.size());
        for (const Impl::Entry& entry : mImpl->Entries)
            descriptors.push_back(entry.Descriptor);
        return descriptors;
    }

    void PluginManager::StartThroughPhase(PluginLoadPhase phase)
    {
        const std::vector<size_t> plan = mImpl->BuildStartPlan(phase);
        std::vector<size_t> startedThisCall;
        for (size_t index : plan)
        {
            Impl::Entry& entry = mImpl->Entries[index];
            if (entry.State == PluginState::Started)
                continue;

            entry.State = PluginState::Loading;
            entry.Diagnostic.clear();
            try
            {
                entry.Module = entry.Factory();
                if (!entry.Module)
                    throw std::runtime_error("Factory anchor returned null");
                entry.State = PluginState::Loaded;
                PluginContext context(
                    entry.Descriptor,
                    mImpl->Environment,
                    mImpl->Services);
                entry.Module->Start(context);
                entry.State = PluginState::Started;
                entry.StartOrder = mImpl->NextStartOrder++;
                mImpl->ActiveOrder.push_back(index);
                startedThisCall.push_back(index);
            }
            catch (...)
            {
                const std::exception_ptr failure = std::current_exception();
                mImpl->RollBackCurrentStart(entry, failure);
                mImpl->RollBackStarted(startedThisCall);
                throw PluginLifecycleError(
                    entry.Descriptor.Id,
                    "start",
                    entry.Diagnostic);
            }
        }
    }

    void PluginManager::StopAll()
    {
        PluginId firstFailureId;
        std::string firstFailure;
        for (auto index = mImpl->ActiveOrder.rbegin();
            index != mImpl->ActiveOrder.rend();
            ++index)
        {
            Impl::Entry& entry = mImpl->Entries[*index];
            if (entry.State != PluginState::Started)
                continue;
            entry.State = PluginState::Stopping;
            entry.Diagnostic.clear();
            try
            {
                PluginContext context(
                    entry.Descriptor,
                    mImpl->Environment,
                    mImpl->Services);
                entry.Module->Stop(context);
                entry.State = PluginState::Unloaded;
            }
            catch (...)
            {
                entry.Diagnostic = DescribeException(std::current_exception());
                entry.State = PluginState::Failed;
                if (firstFailure.empty())
                {
                    firstFailureId = entry.Descriptor.Id;
                    firstFailure = entry.Diagnostic;
                }
            }
            mImpl->Services.RevokeOwner(entry.Descriptor.Id);
            entry.Module.reset();
            entry.StartOrder = 0;
        }
        mImpl->ActiveOrder.clear();
        if (!firstFailure.empty())
            throw PluginLifecycleError(firstFailureId, "stop", firstFailure);
    }

    const PluginHostEnvironment& PluginManager::GetEnvironment() const noexcept
    {
        return mImpl->Environment;
    }

    PluginServiceRegistry& PluginManager::GetServices() noexcept
    {
        return mImpl->Services;
    }

    const PluginServiceRegistry& PluginManager::GetServices() const noexcept
    {
        return mImpl->Services;
    }
}
