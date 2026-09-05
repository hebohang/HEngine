#pragma once

#include "Runtime/AI/BehaviorTree/BTActionNode.h"

#include <string>

namespace HEngine
{
    class BTDeath : public BTActionNode
    {
    public:
        explicit BTDeath(const std::string& deathParam = "bDead")
            : mDeathParam(deathParam)
        {
        }

        BTNodeStatus Tick(float deltaTime, Blackboard& blackboard, Entity entity) override;

    private:
        std::string mDeathParam;
    };
}
