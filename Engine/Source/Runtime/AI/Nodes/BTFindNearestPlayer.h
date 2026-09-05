#pragma once

#include "Runtime/AI/BehaviorTree/BTActionNode.h"

namespace HEngine
{
    class BTFindNearestPlayer : public BTActionNode
    {
    public:
        BTNodeStatus Tick(float deltaTime, Blackboard& blackboard, Entity entity) override;
    };
}
