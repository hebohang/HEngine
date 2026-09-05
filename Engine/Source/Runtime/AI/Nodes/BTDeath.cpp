#include "Hepch.h"

#include "Runtime/AI/Nodes/BTDeath.h"

#include "Runtime/AI/Blackboard/Blackboard.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"
#include "Runtime/EcsFramework/Component/Character/CharacterMovementComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Level/Level.h"

namespace HEngine
{
    BTNodeStatus BTDeath::Tick(float deltaTime, Blackboard& blackboard, Entity entity)
    {
        (void)deltaTime;
        if (!entity)
            return BTNodeStatus::Failure;

        if (entity.HasComponent<AnimatorComponent>())
        {
            auto& animator = entity.GetComponent<AnimatorComponent>();
            animator.SetBool(mDeathParam, true);
        }

        if (entity.HasComponent<CharacterMovementComponent>())
        {
            auto& movement = entity.GetComponent<CharacterMovementComponent>();
            movement.RuntimeDesiredVelocity = glm::vec3(0.0f);
            movement.RuntimeHasExternalVelocity = true;
        }

        blackboard.SetBool("bIsDead", true);

        if (entity.HasComponent<AnimatorComponent>() && entity.GetComponent<AnimatorComponent>().GetProgress() >= 0.95f)
        {
            if (Level* level = entity.GetLevel())
                level->DestroyEntity(entity);
            return BTNodeStatus::Success;
        }

        return BTNodeStatus::Running;
    }
}
