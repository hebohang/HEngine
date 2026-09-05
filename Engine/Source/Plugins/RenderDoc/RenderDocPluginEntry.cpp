#include "Runtime/Core/Plugins/PluginApi.h"
#include "Runtime/Core/Plugins/Services/RenderCaptureService.h"
#include "Plugins/RenderDoc/RenderDoc/RenderDocModule.h"

namespace HEngine
{
    namespace
    {
        class RenderDocPluginModule final : public IPluginModule
        {
        public:
            void Start(PluginContext& context) override
            {
                mService = CreateRef<RenderDocCaptureService>();
                mService->Initialize();
                mRegistration = context.RegisterService<IRenderCaptureService>(
                    c_RenderCaptureServiceId,
                    mService);
            }

            void Stop(PluginContext&) override
            {
                mRegistration.Revoke();
                if (mService)
                    mService->Shutdown();
                mService.reset();
            }

        private:
            Ref<RenderDocCaptureService> mService;
            PluginServiceHandle mRegistration;
        };
    }

    Scope<IPluginModule> HEngineCreateRenderDocPluginModule()
    {
        return CreateScope<RenderDocPluginModule>();
    }
}
