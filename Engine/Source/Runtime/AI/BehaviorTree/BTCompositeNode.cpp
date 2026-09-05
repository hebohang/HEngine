#include "Hepch.h"

#include "Runtime/AI/BehaviorTree/BTCompositeNode.h"
#include "Runtime/AI/Blackboard/Blackboard.h"

namespace HEngine
{
    void BTCompositeNode::AddChild(Scope<BTNode> child)
    {
        if (child)
            mChildren.emplace_back(std::move(child));
    }

    BTNodeStatus BTSelector::Tick(float deltaTime, Blackboard& blackboard, Entity entity)
    {
        (void)deltaTime;
        if (mChildren.empty())
            return BTNodeStatus::Failure;

        if (mCurrentIndex < 0)
            mCurrentIndex = 0;

        for (size_t i = static_cast<size_t>(mCurrentIndex); i < mChildren.size(); ++i)
        {
            BTNodeStatus status = mChildren[i]->Tick(deltaTime, blackboard, entity);
            if (status == BTNodeStatus::Success)
            {
                mCurrentIndex = 0;
                return BTNodeStatus::Success;
            }

            if (status == BTNodeStatus::Running)
            {
                mCurrentIndex = static_cast<int>(i);
                return BTNodeStatus::Running;
            }
        }

        mCurrentIndex = 0;
        return BTNodeStatus::Failure;
    }

    void BTSelector::Reset()
    {
        mCurrentIndex = 0;
        for (const Scope<BTNode>& child : mChildren)
        {
            if (child)
                child->Reset();
        }
    }

    BTNodeStatus BTSequence::Tick(float deltaTime, Blackboard& blackboard, Entity entity)
    {
        (void)deltaTime;
        if (mChildren.empty())
            return BTNodeStatus::Success;

        if (mCurrentIndex < 0)
            mCurrentIndex = 0;

        for (size_t i = static_cast<size_t>(mCurrentIndex); i < mChildren.size(); ++i)
        {
            BTNodeStatus status = mChildren[i]->Tick(deltaTime, blackboard, entity);
            if (status == BTNodeStatus::Failure)
            {
                mCurrentIndex = 0;
                return BTNodeStatus::Failure;
            }

            if (status == BTNodeStatus::Running)
            {
                mCurrentIndex = static_cast<int>(i);
                return BTNodeStatus::Running;
            }
        }

        mCurrentIndex = 0;
        return BTNodeStatus::Success;
    }

    void BTSequence::Reset()
    {
        mCurrentIndex = 0;
        for (const Scope<BTNode>& child : mChildren)
        {
            if (child)
                child->Reset();
        }
    }
}
