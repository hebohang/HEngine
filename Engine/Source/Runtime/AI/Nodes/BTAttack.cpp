#include "Hepch.h"

#include "Runtime/AI/Nodes/BTAttack.h"

#include "Runtime/AI/Blackboard/Blackboard.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"

namespace HEngine
{
    BTNodeStatus BTAttack::Tick(float deltaTime, Blackboard& blackboard, Entity entity)
    {
        if (!entity || !entity.HasComponent<AnimatorComponent>())
            return BTNodeStatus::Failure;

        auto& animator = entity.GetComponent<AnimatorComponent>();

        if (mCooldownTimer > 0.0f)
        {
            mCooldownTimer -= deltaTime;
            if (mCooldownTimer < 0.0f)
                mCooldownTimer = 0.0f;
            return BTNodeStatus::Running;
        }

        if (!mAttacking)
        {
            animator.SetTrigger(mAttackTrigger);
            blackboard.SetFloat("AttackDamage", mDamage);
            mAttacking = true;
            return BTNodeStatus::Running;
        }

        if (animator.GetProgress() >= 0.9f)
        {
            mAttacking = false;
            mCooldownTimer = mCooldown;
            return BTNodeStatus::Success;
        }

        return BTNodeStatus::Running;
    }

    void BTAttack::Reset()
    {
        mCooldownTimer = 0.0f;
        mAttacking = false;
    }
}
