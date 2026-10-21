#include "Hepch.h"
#include "NativeScriptComponent.h"
#include "Runtime/EcsFramework/Entity/ScriptableEntity.h"

#include <stdexcept>
#include <unordered_map>

namespace HEngine
{
    namespace
    {
        auto& NativeTypes()
        {
            static std::unordered_map<std::string, NativeScriptComponent::Binder> types;
            return types;
        }
    }

    NativeScriptComponent::NativeScriptComponent(const NativeScriptComponent& other)
        : Type(other.Type), Properties(other.Properties),
          InstantiateScript(other.InstantiateScript), DestroyScript(other.DestroyScript) {}

    NativeScriptComponent& NativeScriptComponent::operator=(const NativeScriptComponent& other)
    {
        if (this != &other)
        {
            Stop();
            Type = other.Type;
            Properties = other.Properties;
            InstantiateScript = other.InstantiateScript;
            DestroyScript = other.DestroyScript;
        }
        return *this;
    }

    NativeScriptComponent::~NativeScriptComponent() { Stop(); }

    NativeScriptComponent::NativeScriptComponent(NativeScriptComponent&& other) noexcept
        : Type(std::move(other.Type)), Properties(std::move(other.Properties)),
          Instance(std::exchange(other.Instance, nullptr)),
          InstantiateScript(other.InstantiateScript), DestroyScript(other.DestroyScript) {}

    NativeScriptComponent& NativeScriptComponent::operator=(NativeScriptComponent&& other) noexcept
    {
        if (this != &other)
        {
            Stop();
            Type = std::move(other.Type);
            Properties = std::move(other.Properties);
            Instance = std::exchange(other.Instance, nullptr);
            InstantiateScript = other.InstantiateScript;
            DestroyScript = other.DestroyScript;
        }
        return *this;
    }

    void NativeScriptComponent::Stop() noexcept
    {
        // Clear first so recursive entity removal cannot destroy this instance twice.
        ScriptableEntity* instance = std::exchange(Instance, nullptr);
        if (!instance) return;
        try { instance->OnDestroy(); }
        catch (const std::exception& error) { Log::WriteFallbackError("Native OnDestroy failed", error.what()); }
        catch (...) { Log::WriteFallbackError("Native OnDestroy failed"); }
        delete instance;
    }

    void NativeScriptComponent::RegisterType(const std::string& name, Binder binder)
    {
        if (name.empty() || !binder) throw std::invalid_argument("Native script registration requires a name and binder.");
        const auto [it, inserted] = NativeTypes().emplace(name, binder);
        if (!inserted && it->second != binder) throw std::logic_error("Duplicate native script type: " + name);
    }

    void NativeScriptComponent::ResolveType()
    {
        if (Type.empty())
        {
            if (!InstantiateScript) throw std::runtime_error("NativeScriptComponent has neither Type nor Bind<T>().");
            return;
        }
        const auto it = NativeTypes().find(Type);
        if (it == NativeTypes().end()) throw std::runtime_error("Unregistered native script type: " + Type);
        it->second(*this);
    }

}
