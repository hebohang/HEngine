#pragma once

#include "Runtime/EcsFramework/System/System.h"

namespace HEngine
{
    // Returns the simulated interval to deliver to a tree, or zero when its
    // authored cadence has not elapsed. The remainder stays in accumulator.
    [[nodiscard]] float AdvanceBehaviorTreeTickCadence(
        float& accumulator, float fixedDt, float tickInterval) noexcept;

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
