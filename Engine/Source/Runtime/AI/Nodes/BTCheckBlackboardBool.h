#pragma once

#include "Runtime/AI/BehaviorTree/BTConditionNode.h"

#include <string>

namespace HEngine
{
    class BTCheckBlackboardBool : public BTConditionNode
    {
    public:
        BTCheckBlackboardBool(const std::string& key, bool expectedValue = true)
            : mKey(key), mExpectedValue(expectedValue)
        {
        }

        BTNodeStatus Tick(float deltaTime, Blackboard& blackboard, Entity entity) override;

    private:
        std::string mKey;
        bool mExpectedValue = true;
    };
}
