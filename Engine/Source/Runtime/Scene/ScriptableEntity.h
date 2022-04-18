#pragma once

#include "Entity.h"

namespace HEngine
{
    class ScriptableEntity
    {
    public:
        virtual ~ScriptableEntity() {}

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
        friend class Scene;
    };
}
