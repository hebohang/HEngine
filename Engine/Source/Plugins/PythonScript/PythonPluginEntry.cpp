#include "PythonRuntime.h"
#include "PythonBackend.h"
#include "Runtime/Core/Plugins/PluginApi.h"
#include "Runtime/Scripting/ScriptBackendRegistry.h"

namespace HEngine
{
    namespace
    {
        class PythonScriptPluginModule final : public IPluginModule
        {
        public:
            void Start(PluginContext&) override
            {
                ScriptEngine::InstallRuntime(std::make_shared<PythonRuntime>());
                try { ScriptBackendRegistry::Register(std::make_shared<PythonBackend>()); }
                catch (...) { ScriptEngine::RemoveRuntime(); throw; }
            }
            void Stop(PluginContext&) override
            {
                if (PythonBackend::HasLiveInstances()) throw std::logic_error("Python plugin still has live script instances.");
                auto backend = ScriptBackendRegistry::Unregister("he.script.python");
                backend.reset();
                ScriptEngine::RemoveRuntime();
            }
        };
    }

    Scope<IPluginModule> HEngineCreatePythonScriptPluginModule()
    {
        return CreateScope<PythonScriptPluginModule>();
    }
}
