#pragma once

#include "Runtime/AI/BehaviorTree/BTNode.h"

namespace HEngine
{
    class BTConditionNode : public BTNode
    {
    public:
        virtual ~BTConditionNode() = default;
        virtual BTNodeStatus Tick(float deltaTime, Blackboard& blackboard, Entity entity) = 0;
    };
}
