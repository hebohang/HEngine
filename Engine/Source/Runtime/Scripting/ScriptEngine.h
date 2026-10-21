#pragma once

#include "Runtime/Core/Templates/PublicSingleton.h"

#include <memory>
#include <string>

namespace HEngine
{
    class UUID;
    class Entity;
    struct RmlUiEventPayloadData;

    enum class LegacyScriptCallback { Start, Update, FixedUpdate, UpdatePaused, UI, Stop };

    // Compatibility for existing Python assets. No runtime objects cross this
    // interface. Calls execute synchronously on the engine's owning thread.
    class ILegacyScriptRuntime
    {
    public:
        virtual ~ILegacyScriptRuntime() = default;
        virtual void Init() = 0;
        virtual void Shutdown() = 0;
        virtual bool IsInitialized() const = 0;
        virtual bool HasInstances() const = 0;
        virtual bool RegisterModule(const UUID&, const std::string&) = 0;
        virtual std::string GetLastRegistrationError(const UUID&) const = 0;
        virtual bool IsModuleRegistered(const UUID&) const = 0;
        virtual bool IsClassRegistered(const UUID&) const = 0;
        virtual std::string GetPyClassName(const UUID&) const = 0;
        virtual void ReloadModule(const UUID&, bool recreateClass) = 0;
        virtual void UnregisterModule(const UUID&) = 0;
        virtual void Invoke(const UUID&, LegacyScriptCallback, Entity*, float) = 0;
        virtual void DispatchUI(const UUID&, const std::string& legacySource,
            const RmlUiEventPayloadData&) = 0;
        virtual void Execute(const std::string&) = 0;
    };

    class ScriptEngine final : public PublicSingleton<ScriptEngine>
    {
    public:
        void Init();
        void Shutdown();
        static void InstallRuntime(std::shared_ptr<ILegacyScriptRuntime> runtime);
        static void RemoveRuntime();
        static bool IsInitialized();
        static bool IsAvailable();
        static bool RegisterModule(const UUID&, const std::string& scriptPath);
        static std::string GetLastRegistrationError(const UUID&);
        static bool IsModuleRegistered(const UUID&);
        static bool IsClassRegistered(const UUID&);
        static std::string GetPyClassName(const UUID&);
        static void ReloadModule(const UUID&);
        static void ReloadModuleAndClass(const UUID&);
        static void UnregisterModule(const UUID&);
        static void Invoke(const UUID&, LegacyScriptCallback, Entity* entity = nullptr, float seconds = 0);
        static void DispatchUI(const UUID&, const std::string& legacySource, const RmlUiEventPayloadData&);
        static void Execute(const std::string&);
    };
}
