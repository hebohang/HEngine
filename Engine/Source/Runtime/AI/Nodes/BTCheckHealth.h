#pragma once

#include "Runtime/AI/BehaviorTree/BTConditionNode.h"
#include "Runtime/AI/Nodes/BTCheckDistance.h"

namespace HEngine
{
    class BTCheckHealth : public BTConditionNode
    {
    public:
        BTCheckHealth(float threshold = 0.0f, BTCompareOp op = BTCompareOp::LessEqual)
            : mThreshold(threshold), mOp(op)
        {
        }

        BTNodeStatus Tick(float deltaTime, Blackboard& blackboard, Entity entity) override;

    private:
        float mThreshold = 0.0f;
        BTCompareOp mOp = BTCompareOp::LessEqual;
    };
}
