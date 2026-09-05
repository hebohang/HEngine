#pragma once

#include "Runtime/AI/BehaviorTree/BTActionNode.h"

#include <string>

namespace HEngine
{
    class BTAttack : public BTActionNode
    {
    public:
        BTAttack(float cooldown = 1.5f, float damage = 10.0f, const std::string& attackTrigger = "bAttacking")
            : mCooldown(cooldown), mDamage(damage), mAttackTrigger(attackTrigger)
        {
        }

        BTNodeStatus Tick(float deltaTime, Blackboard& blackboard, Entity entity) override;
        void Reset() override;

    private:
        float mCooldown = 1.5f;
        float mDamage = 10.0f;
        std::string mAttackTrigger;
        float mCooldownTimer = 0.0f;
        bool mAttacking = false;
    };
}
