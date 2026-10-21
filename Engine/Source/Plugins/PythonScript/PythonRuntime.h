#pragma once

#include "Runtime/Scripting/ScriptEngine.h"

#include <thread>

namespace HEngine
{
    class PythonRuntime final : public ILegacyScriptRuntime
    {
    public:
        void Init() override;
        void Shutdown() override;
        bool IsInitialized() const override;
        bool HasInstances() const override;
        bool RegisterModule(const UUID&, const std::string&) override;
        std::string GetLastRegistrationError(const UUID&) const override;
        bool IsModuleRegistered(const UUID&) const override;
        bool IsClassRegistered(const UUID&) const override;
        std::string GetPyClassName(const UUID&) const override;
        void ReloadModule(const UUID&, bool recreateClass) override;
        void UnregisterModule(const UUID&) override;
        void Invoke(const UUID&, LegacyScriptCallback, Entity*, float) override;
        void DispatchUI(const UUID&, const std::string&, const RmlUiEventPayloadData&) override;
        void Execute(const std::string&) override;

    private:
        void CheckThread() const;
        template<typename F> void Guard(F&& function);
        std::thread::id mThread;
    };
}
