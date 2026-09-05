#include "Hepch.h"

#include "Runtime/AI/Nodes/BTCheckHealth.h"

#include "Runtime/AI/Blackboard/Blackboard.h"

namespace HEngine
{
    namespace
    {
        bool CompareHealth(BTCompareOp op, float lhs, float rhs)
        {
            switch (op)
            {
            case BTCompareOp::Less:
                return lhs < rhs;
            case BTCompareOp::Greater:
                return lhs > rhs;
            case BTCompareOp::LessEqual:
                return lhs <= rhs;
            case BTCompareOp::GreaterEqual:
                return lhs >= rhs;
            default:
                return false;
            }
        }
    }

    BTNodeStatus BTCheckHealth::Tick(float deltaTime, Blackboard& blackboard, Entity entity)
    {
        (void)deltaTime;
        (void)entity;
        const float health = blackboard.GetFloat("Health", 0.0f);
        return CompareHealth(mOp, health, mThreshold) ? BTNodeStatus::Success : BTNodeStatus::Failure;
    }
}
