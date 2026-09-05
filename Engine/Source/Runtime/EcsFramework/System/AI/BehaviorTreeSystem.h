#pragma once

#include "Runtime/EcsFramework/System/System.h"

namespace HEngine
{
    class BehaviorTreeSystem : public System
    {
    public:
        BehaviorTreeSystem(Level* level)
            : System(level)
        {
        }

        virtual ~BehaviorTreeSystem() = default;

        void OnRuntiemStart() override;
        void OnUpdateRuntime(Timestep ts) override;
        void OnRuntimeStop() override;
    };
}
