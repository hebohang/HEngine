#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

namespace HEngine
{
    // Forward declaration
    class ScriptableEntity;
    REFLECTION_TYPE(NativeScriptComponent)
    CLASS(NativeScriptComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(NativeScriptComponent);
    public:
        std::string Type;
        // The JSON payload is reflected/serialized, but is not a pybind value.
        PROPERTY(NoPython)
        Json Properties = Json::object();

        NativeScriptComponent() = default;
        NativeScriptComponent(const NativeScriptComponent& other);
        NativeScriptComponent& operator=(const NativeScriptComponent& other);
        NativeScriptComponent(NativeScriptComponent&& other) noexcept;
        NativeScriptComponent& operator=(NativeScriptComponent&& other) noexcept;
        ~NativeScriptComponent() override;
        void Stop() noexcept;
        void ResolveType();

        using Binder = void(*)(NativeScriptComponent&);
        static void RegisterType(const std::string& name, Binder binder);

        PROPERTY(Disable)
        ScriptableEntity* Instance = nullptr;

        PROPERTY(Disable)
        ScriptableEntity* (*InstantiateScript)() = nullptr;
        PROPERTY(Disable)
        void(*DestroyScript)(NativeScriptComponent*) = nullptr;

        template<typename T>
        void Bind()
        {
            Stop();
            InstantiateScript = []() { return static_cast<ScriptableEntity*>(new T()); };
            DestroyScript = [](NativeScriptComponent* nsc) { nsc->Stop(); };
        }
    };
    REGISTER_COMPONENT_TYPE(NativeScriptComponent)
}
