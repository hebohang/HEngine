#include "Hepch.h"

#include "Runtime/AI/BehaviorTree/BTDecoratorNode.h"
#include "Runtime/AI/Blackboard/Blackboard.h"

namespace HEngine
{
    void BTDecoratorNode::SetChild(Scope<BTNode> child)
    {
        mChild = std::move(child);
    }

    BTNodeStatus BTInverter::Tick(float deltaTime, Blackboard& blackboard, Entity entity)
    {
        (void)deltaTime;
        if (!mChild)
            return BTNodeStatus::Failure;

        BTNodeStatus status = mChild->Tick(deltaTime, blackboard, entity);
        if (status == BTNodeStatus::Success)
            return BTNodeStatus::Failure;
        if (status == BTNodeStatus::Failure)
            return BTNodeStatus::Success;
        return BTNodeStatus::Running;
    }

    void BTInverter::Reset()
    {
        if (mChild)
            mChild->Reset();
    }

    BTNodeStatus BTRepeat::Tick(float deltaTime, Blackboard& blackboard, Entity entity)
    {
        (void)deltaTime;
        if (!mChild)
            return BTNodeStatus::Failure;

        BTNodeStatus status = mChild->Tick(deltaTime, blackboard, entity);
        if (status == BTNodeStatus::Failure)
            return BTNodeStatus::Failure;

        if (status == BTNodeStatus::Running)
            return BTNodeStatus::Running;

        ++mCurrentCount;
        if (mMaxCount >= 0 && mCurrentCount >= mMaxCount)
            return BTNodeStatus::Success;

        mChild->Reset();
        return BTNodeStatus::Running;
    }

    void BTRepeat::Reset()
    {
        mCurrentCount = 0;
        if (mChild)
            mChild->Reset();
    }

    BTNodeStatus BTAlwaysSucceed::Tick(float deltaTime, Blackboard& blackboard, Entity entity)
    {
        (void)deltaTime;
        if (!mChild)
            return BTNodeStatus::Success;

        BTNodeStatus status = mChild->Tick(deltaTime, blackboard, entity);
        if (status == BTNodeStatus::Running)
            return BTNodeStatus::Running;

        return BTNodeStatus::Success;
    }

    void BTAlwaysSucceed::Reset()
    {
        if (mChild)
            mChild->Reset();
    }
}
