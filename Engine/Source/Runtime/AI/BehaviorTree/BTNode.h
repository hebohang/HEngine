#pragma once

#include "Runtime/EcsFramework/Entity/Entity.h"

#include <string>

namespace HEngine
{
    class Blackboard;

    enum class BTNodeStatus
    {
        Success,
        Failure,
        Running
    };

    class BTNode
    {
    public:
        virtual ~BTNode() = default;
        virtual BTNodeStatus Tick(float deltaTime, Blackboard& blackboard, Entity entity) = 0;
        virtual void Reset() {}

        std::string mName;
    };
}
