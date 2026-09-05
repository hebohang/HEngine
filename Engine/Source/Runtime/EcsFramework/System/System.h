#pragma once

#include "Runtime/Core/Timestep.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/Camera/EditorCamera.h"

namespace HEngine
{
    class System
    {
    public:
        System() = delete;
        System(Level* level) : mLevel(level) {}
        virtual ~System() = default;
    public:
#if WITH_EDITOR
        virtual void OnUpdateEditor(Timestep ts, EditorCamera& camera) {};
        virtual void OnImGuiRenderEditor() {}
		[[nodiscard]] virtual bool RequiresContinuousEditorUpdate() const { return false; }
#endif
        virtual void OnUpdateRuntime(Timestep ts) {}
        virtual void OnRuntiemStart() {}
        virtual void OnRuntimeStop() {}
        virtual void OnImGuiRender() {}
        [[nodiscard]] virtual bool ShouldUpdateWhenRuntimePaused() const { return false; }
    protected:
        Level* mLevel = nullptr;
    };
}
