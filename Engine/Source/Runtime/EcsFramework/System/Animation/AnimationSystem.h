#pragma once

#include "Runtime/EcsFramework/System/System.h"
#include "Runtime/EcsFramework/Level/Level.h"

namespace HEngine
{
    class Entity;

    class AnimationSystem : public System
    {
    public:
        AnimationSystem(Level* level);
        virtual ~AnimationSystem() = default;
    public:
        void OnRuntiemStart()              override;
        void OnUpdateRuntime(Timestep ts)  override;
        void OnRuntimeStop()               override;
#if WITH_EDITOR
        void OnUpdateEditor(Timestep ts, EditorCamera& camera) override;
#endif
    private:
        void TickEntity(Entity entity, Timestep ts, bool bRuntime);
    };
}

