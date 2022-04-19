#pragma once

#include "Runtime/Core/Timestep.h"
#include "Runtime/EcsFramework/Level/Level.h"

namespace HEngine
{
    class System
    {
    public:
        System() = delete;
        System(Level* level) : mLevel(level) {}
        virtual ~System() = default;
    public:
        virtual void OnUpdate(Timestep ts) {}
        virtual void OnRuntiemStart() {}
        virtual void OnRuntimeStop() {}
    protected:
        Level* mLevel = nullptr;
    };
}