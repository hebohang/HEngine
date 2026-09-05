#include "Hepch.h"

#include "Runtime/AI/Nodes/BTCheckBlackboardBool.h"

#include "Runtime/AI/Blackboard/Blackboard.h"

namespace HEngine
{
    BTNodeStatus BTCheckBlackboardBool::Tick(float deltaTime, Blackboard& blackboard, Entity entity)
    {
        (void)deltaTime;
        (void)entity;
        const bool value = blackboard.GetBool(mKey, !mExpectedValue);
        return value == mExpectedValue ? BTNodeStatus::Success : BTNodeStatus::Failure;
    }
}
