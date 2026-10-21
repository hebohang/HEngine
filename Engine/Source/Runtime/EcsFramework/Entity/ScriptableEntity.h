#pragma once

#include "Runtime/EcsFramework/Entity/Entity.h"

namespace HEngine
{
    class ScriptableEntity
    {
    public:
        virtual ~ScriptableEntity() {}
        virtual void LoadProperties(const Json& properties) {}
        virtual Json SaveProperties() const { return Json::object(); }

        template<typename T>
        T& GetComponent()
        {
            return mEntity.GetComponent<T>();
        }
    protected:
        virtual void OnCreate() {}
        virtual void OnDestroy() {}
        virtual void OnUpdate(Timestep ts) {}
    private:
        Entity mEntity;
        friend class NativeScriptSystem;
        friend class NativeScriptComponent;
    };
}
