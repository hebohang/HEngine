#pragma once

#include "Runtime/AI/BehaviorTree/BTActionNode.h"

namespace HEngine
{
    class BTMoveDirectlyToTarget : public BTActionNode
    {
    public:
        explicit BTMoveDirectlyToTarget(float arriveThreshold = 1.5f)
            : mArriveThreshold(arriveThreshold)
        {
        }

        BTNodeStatus Tick(float deltaTime, Blackboard& blackboard, Entity entity) override;

    private:
        float mArriveThreshold = 1.5f;
    };
}
