#include "ScriptEngine.h"

#include <stdexcept>
#include <utility>

namespace HEngine
{
    namespace
    {
        std::shared_ptr<ILegacyScriptRuntime> sRuntime;
        bool sInitializationRequested = false;
        constexpr const char* c_MissingRuntime =
            "Python script plugin 'he.script.python' is unavailable on this host. Enable it in Config/Plugins.json.";

        std::shared_ptr<ILegacyScriptRuntime> RequireRuntime()
        {
            if (!sRuntime || !sRuntime->IsInitialized())
                throw std::runtime_error(c_MissingRuntime);
            return sRuntime;
        }
    }

    void ScriptEngine::Init()
    {
        if (sRuntime && !sRuntime->IsInitialized())
            sRuntime->Init();
        sInitializationRequested = true;
    }

    void ScriptEngine::Shutdown()
    {
        if (sRuntime)
            sRuntime->Shutdown();
        sInitializationRequested = false;
    }

    void ScriptEngine::InstallRuntime(std::shared_ptr<ILegacyScriptRuntime> runtime)
    {
        if (!runtime || sRuntime)
            throw std::logic_error("Python script runtime registration is null or duplicated.");
        if (sInitializationRequested)
            runtime->Init();
        sRuntime = std::move(runtime);
    }

    void ScriptEngine::RemoveRuntime()
    {
        if (sRuntime && sRuntime->HasInstances())
            throw std::logic_error("Cannot stop Python plugin while script instances are alive.");
        if (sRuntime)
            sRuntime->Shutdown();
        sRuntime.reset();
    }

    bool ScriptEngine::IsAvailable() { return bool(sRuntime); }
    bool ScriptEngine::IsInitialized() { return sRuntime && sRuntime->IsInitialized(); }
    bool ScriptEngine::RegisterModule(const UUID& id, const std::string& path)
    {
        return IsInitialized() && RequireRuntime()->RegisterModule(id, path);
    }
    std::string ScriptEngine::GetLastRegistrationError(const UUID& id)
    {
        return IsInitialized() ? RequireRuntime()->GetLastRegistrationError(id) : c_MissingRuntime;
    }
    bool ScriptEngine::IsModuleRegistered(const UUID& id)
    {
        return IsInitialized() && RequireRuntime()->IsModuleRegistered(id);
    }
    bool ScriptEngine::IsClassRegistered(const UUID& id)
    {
        return IsInitialized() && RequireRuntime()->IsClassRegistered(id);
    }
    std::string ScriptEngine::GetPyClassName(const UUID& id) { return RequireRuntime()->GetPyClassName(id); }
    void ScriptEngine::ReloadModule(const UUID& id) { RequireRuntime()->ReloadModule(id, false); }
    void ScriptEngine::ReloadModuleAndClass(const UUID& id) { RequireRuntime()->ReloadModule(id, true); }
    void ScriptEngine::UnregisterModule(const UUID& id)
    {
        if (IsInitialized()) RequireRuntime()->UnregisterModule(id);
    }
    void ScriptEngine::Invoke(const UUID& id, LegacyScriptCallback callback, Entity* entity, float seconds)
    {
        RequireRuntime()->Invoke(id, callback, entity, seconds);
    }
    void ScriptEngine::DispatchUI(const UUID& id, const std::string& legacySource, const RmlUiEventPayloadData& data)
    {
        RequireRuntime()->DispatchUI(id, legacySource, data);
    }
    void ScriptEngine::Execute(const std::string& code) { RequireRuntime()->Execute(code); }
}
