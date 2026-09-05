#include "Runtime/Core/Plugins/PluginApi.h"

namespace HEngine
{
    namespace
    {
        class ExportersPluginModule final : public IPluginModule
        {
        public:
            void Start(PluginContext&) override {}
            void Stop(PluginContext&) override {}
        };
    }

    Scope<IPluginModule> HEngineCreateExportersPluginModule()
    {
        return CreateScope<ExportersPluginModule>();
    }
}
