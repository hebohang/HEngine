#pragma once

#include "Runtime/EcsFramework/System/System.h"

namespace HEngine
{
    class CameraFollowSystem : public System
    {
    public:
        CameraFollowSystem(Level* level)
            : System(level)
        {
        }

        virtual ~CameraFollowSystem() = default;

        void OnUpdateRuntime(Timestep ts) override;
        [[nodiscard]] bool ShouldUpdateWhenRuntimePaused() const override { return true; }
        // Follow motion belongs to the runtime scene copy. Editing must retain
        // the authored camera transform, including during zero-time warmup.
    };
}
