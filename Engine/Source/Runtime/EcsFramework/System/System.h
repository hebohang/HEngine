#pragma once

#include "Runtime/Core/Timestep.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/Renderer/EditorCamera.h"

namespace HEngine
{
    class System
    {
    public:
        System() = delete;
        System(Level* level) : mLevel(level) {}
        virtual ~System() = default;
    public:
        virtual void OnUpdateEditor(Timestep ts, EditorCamera& camera) {};
        virtual void OnUpdateRuntime(Timestep ts) {}
        virtual void OnRuntiemStart() {}
        virtual void OnRuntimeStop() {}
    protected:
        Level* mLevel = nullptr;
    };
}