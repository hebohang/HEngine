#include "CSharpBackend.h"
#include "Runtime/Core/Plugins/PluginApi.h"
#include "Runtime/Scripting/ScriptBackendRegistry.h"

namespace HEngine
{
    namespace
    {
        class CSharpScriptPluginModule final : public IPluginModule
        {
        public:
            void Start(PluginContext&) override { ScriptBackendRegistry::Register(std::make_shared<CSharpBackend>()); }
            void Stop(PluginContext&) override
            {
                auto backend = std::static_pointer_cast<CSharpBackend>(ScriptBackendRegistry::Unregister("he.script.csharp"));
                if (backend) backend->Shutdown();
            }
        };
    }
    Scope<IPluginModule> HEngineCreateCSharpScriptPluginModule() { return CreateScope<CSharpScriptPluginModule>(); }
}
