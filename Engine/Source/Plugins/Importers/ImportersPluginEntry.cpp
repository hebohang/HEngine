#include "Runtime/Core/Plugins/PluginApi.h"
#include "Runtime/Core/Plugins/Services/MeshImporterService.h"
#include "Plugins/Importers/MeshImporter/MeshImporter.h"

namespace HEngine
{
    namespace
    {
        class ImportersPluginModule final : public IPluginModule
        {
        public:
            void Start(PluginContext& context) override
            {
                mService = CreateRef<MeshImporter>();
                mRegistration = context.RegisterService<IMeshImporterService>(
                    c_MeshImporterServiceId,
                    mService);
            }

            void Stop(PluginContext&) override
            {
                mRegistration.Revoke();
                mService.reset();
            }

        private:
            Ref<MeshImporter> mService;
            PluginServiceHandle mRegistration;
        };
    }

    Scope<IPluginModule> HEngineCreateImportersPluginModule()
    {
        return CreateScope<ImportersPluginModule>();
    }
}
