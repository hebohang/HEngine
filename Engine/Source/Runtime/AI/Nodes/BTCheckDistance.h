#pragma once

#include "Runtime/AI/BehaviorTree/BTConditionNode.h"

namespace HEngine
{
    enum class BTCompareOp
    {
        Less = 0,
        Greater,
        LessEqual,
        GreaterEqual
    };

    class BTCheckDistance : public BTConditionNode
    {
    public:
        BTCheckDistance(BTCompareOp op = BTCompareOp::Less, float threshold = 0.0f)
            : mOp(op), mThreshold(threshold)
        {
        }

        BTNodeStatus Tick(float deltaTime, Blackboard& blackboard, Entity entity) override;

    private:
        BTCompareOp mOp = BTCompareOp::Less;
        float mThreshold = 0.0f;
    };
}
