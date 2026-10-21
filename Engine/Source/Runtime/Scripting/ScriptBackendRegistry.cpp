#include "ScriptBackendRegistry.h"

#include <stdexcept>

namespace HEngine
{
    namespace
    {
        auto& Backends()
        {
            static std::map<std::string, std::shared_ptr<IScriptBackend>> backends;
            return backends;
        }
    }

    void ScriptBackendRegistry::Register(std::shared_ptr<IScriptBackend> backend)
    {
        if (!backend || backend->Descriptor().Id.empty() || backend->Descriptor().ApiVersion != 1)
            throw std::invalid_argument("Script backend has an invalid identity or interface version.");
        const std::string id = backend->Descriptor().Id;
        if (!Backends().emplace(id, std::move(backend)).second)
            throw std::logic_error("Script backend already registered: " + id);
    }

    std::shared_ptr<IScriptBackend> ScriptBackendRegistry::Unregister(const std::string& id)
    {
        const auto it = Backends().find(id);
        if (it == Backends().end()) return {};
        if (it->second.use_count() != 1)
            throw std::logic_error("Cannot stop script backend while a scene owns it: " + id);
        auto backend = std::move(it->second);
        Backends().erase(it);
        return backend;
    }

    std::shared_ptr<IScriptBackend> ScriptBackendRegistry::Find(const std::string& id)
    {
        const auto it = Backends().find(id);
        return it == Backends().end() ? nullptr : it->second;
    }

    std::vector<ScriptBackendDescriptor> ScriptBackendRegistry::Describe()
    {
        std::vector<ScriptBackendDescriptor> result;
        for (const auto& [id, backend] : Backends()) result.push_back(backend->Descriptor());
        return result;
    }
}
