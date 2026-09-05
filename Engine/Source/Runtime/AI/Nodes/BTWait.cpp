#include "Hepch.h"

#include "Runtime/AI/Nodes/BTWait.h"

#include "Runtime/AI/Blackboard/Blackboard.h"

namespace HEngine
{
    BTNodeStatus BTWait::Tick(float deltaTime, Blackboard& blackboard, Entity entity)
    {
        (void)blackboard;
        (void)entity;

        mTimer += deltaTime;
        if (mTimer >= mWaitTime)
        {
            mTimer = 0.0f;
            return BTNodeStatus::Success;
        }

        return BTNodeStatus::Running;
    }

    void BTWait::Reset()
    {
        mTimer = 0.0f;
    }
}
