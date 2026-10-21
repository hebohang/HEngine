#include "Runtime/Core/Plugins/PluginApi.h"
#include "Runtime/Core/Plugins/Services/MeshImporterService.h"
#include "Plugins/Importers/MeshImporter/MeshImporter.h"
#include "Plugins/Importers/MeshImporter/GltfSceneImporter.h"

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
                mSceneService = CreateRef<GltfSceneImporter>();
                mSceneRegistration = context.RegisterService<ISceneImporterService>(
                    c_SceneImporterServiceId, mSceneService);
            }

            void Stop(PluginContext&) override
            {
                mRegistration.Revoke();
                mSceneRegistration.Revoke();
                mSceneService.reset();
                mService.reset();
            }

        private:
            Ref<MeshImporter> mService;
            PluginServiceHandle mRegistration;
            Ref<GltfSceneImporter> mSceneService;
            PluginServiceHandle mSceneRegistration;
        };
    }

    Scope<IPluginModule> HEngineCreateImportersPluginModule()
    {
        return CreateScope<ImportersPluginModule>();
    }
}
