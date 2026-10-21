#pragma once

#include "ScriptCore.h"

namespace HEngine
{
    // Static and dynamic plugin entry points share this process registry. The
    // registry itself owns no language runtime; only enabled plugins populate it.
    class ScriptBackendRegistry final
    {
    public:
        static void Register(std::shared_ptr<IScriptBackend> backend);
        static std::shared_ptr<IScriptBackend> Unregister(const std::string& id);
        static std::shared_ptr<IScriptBackend> Find(const std::string& id);
        static std::vector<ScriptBackendDescriptor> Describe();
    };
}
