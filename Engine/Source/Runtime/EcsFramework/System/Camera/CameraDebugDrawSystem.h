#pragma once

#include "Runtime/EcsFramework/System/System.h"

namespace HEngine
{
    class CameraDebugDrawSystem : public System
    {
    public:
        CameraDebugDrawSystem(Level* level)
            : System(level)
        {
        }

        virtual ~CameraDebugDrawSystem() = default;

#if WITH_EDITOR
        void OnUpdateEditor(Timestep ts, EditorCamera& camera) override;
#endif
    };
}
