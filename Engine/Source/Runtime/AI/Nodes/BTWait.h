#pragma once

#include "Runtime/AI/BehaviorTree/BTActionNode.h"

namespace HEngine
{
    class BTWait : public BTActionNode
    {
    public:
        explicit BTWait(float waitTime)
            : mWaitTime(waitTime)
        {
        }

        BTNodeStatus Tick(float deltaTime, Blackboard& blackboard, Entity entity) override;
        void Reset() override;

    private:
        float mWaitTime = 0.0f;
        float mTimer = 0.0f;
    };
}
