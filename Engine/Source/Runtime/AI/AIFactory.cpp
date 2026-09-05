#include "Hepch.h"

#include "Runtime/AI/AIFactory.h"

#include "Runtime/AI/BehaviorTree/BTCompositeNode.h"
#include "Runtime/AI/Nodes/BTAttack.h"
#include "Runtime/AI/Nodes/BTCheckDistance.h"
#include "Runtime/AI/Nodes/BTCheckHealth.h"
#include "Runtime/AI/Nodes/BTDeath.h"
#include "Runtime/AI/Nodes/BTFindNearestPlayer.h"
#include "Runtime/AI/Nodes/BTMoveDirectlyToTarget.h"
#include "Runtime/AI/Nodes/BTNavigateToTarget.h"
#include "Runtime/AI/Nodes/BTSetAnimParam.h"

namespace HEngine
{
    namespace
    {
        Scope<BTNode> CreateSharedBaseTree(bool bUseNavigation)
        {
            auto root = CreateScope<BTSelector>();
            root->mName = "Root";

            auto deathSequence = CreateScope<BTSequence>();
            deathSequence->mName = "DeathSequence";
            deathSequence->AddChild(CreateScope<BTCheckHealth>(0.0f, BTCompareOp::LessEqual));
            deathSequence->AddChild(CreateScope<BTDeath>("bDead"));

            auto attackSequence = CreateScope<BTSequence>();
            attackSequence->mName = "AttackSequence";
            attackSequence->AddChild(CreateScope<BTFindNearestPlayer>());
            attackSequence->AddChild(CreateScope<BTCheckDistance>(BTCompareOp::Less, 2.0f));
            attackSequence->AddChild(CreateScope<BTSetAnimParam>("bMoving", false));
            attackSequence->AddChild(CreateScope<BTAttack>(1.5f, 10.0f, "bAttacking"));

            auto chaseSequence = CreateScope<BTSequence>();
            chaseSequence->mName = "ChaseSequence";
            chaseSequence->AddChild(CreateScope<BTFindNearestPlayer>());
            chaseSequence->AddChild(CreateScope<BTSetAnimParam>("bMoving", true));
            chaseSequence->AddChild(CreateScope<BTSetAnimParam>("bAttacking", false));
            if (bUseNavigation)
                chaseSequence->AddChild(CreateScope<BTNavigateToTarget>(1.8f, 1.0f));
            else
                chaseSequence->AddChild(CreateScope<BTMoveDirectlyToTarget>(1.8f));

            root->AddChild(std::move(deathSequence));
            root->AddChild(std::move(attackSequence));
            root->AddChild(std::move(chaseSequence));

            return root;
        }
    }

    Scope<BTNode> CreateSimpleChaseMonsterAI()
    {
        return CreateSharedBaseTree(false);
    }

    Scope<BTNode> CreateNavMeshChaseMonsterAI()
    {
        return CreateSharedBaseTree(true);
    }
}
