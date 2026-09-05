#pragma once

#include "Runtime/AI/BehaviorTree/BTNode.h"

namespace HEngine
{
    class BTActionNode : public BTNode
    {
    public:
        virtual ~BTActionNode() = default;
        virtual BTNodeStatus Tick(float deltaTime, Blackboard& blackboard, Entity entity) = 0;
    };
}
