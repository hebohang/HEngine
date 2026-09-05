#include "Hepch.h"

#include "Runtime/AI/Nodes/BTMoveDirectlyToTarget.h"

#include "Runtime/AI/Blackboard/Blackboard.h"
#include "Runtime/EcsFramework/Component/Character/CharacterMovementComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"

#include <glm/geometric.hpp>

#include <cmath>

namespace HEngine
{
    BTNodeStatus BTMoveDirectlyToTarget::Tick(float deltaTime, Blackboard& blackboard, Entity entity)
    {
		if (!entity || !entity.HasComponent<TransformComponent>())
			return BTNodeStatus::Failure;

		Level* level = entity.GetLevel();
		const glm::vec3 worldPosition = level->GetWorldTranslation(entity);
		const glm::vec3 target = blackboard.GetVec3("TargetPosition", worldPosition);
		glm::vec3 offset = target - worldPosition;
		offset.y = 0.0f;

        const float dist = glm::length(offset);
        blackboard.SetFloat("DistToTarget", dist);
        if (dist < mArriveThreshold)
        {
            if (entity.HasComponent<CharacterMovementComponent>())
            {
                auto& movement = entity.GetComponent<CharacterMovementComponent>();
                movement.RuntimeDesiredVelocity = glm::vec3(0.0f);
                movement.RuntimeHasExternalVelocity = false;
            }
            return BTNodeStatus::Success;
        }

        glm::vec3 dir = offset;
        if (glm::length2(dir) > 0.0001f)
            dir = glm::normalize(dir);
        else
            dir = glm::vec3(0.0f);

        if (entity.HasComponent<CharacterMovementComponent>())
        {
            auto& movement = entity.GetComponent<CharacterMovementComponent>();
            movement.RuntimeDesiredVelocity = dir * movement.MaxSpeed;
            movement.RuntimeHasExternalVelocity = true;
		}
		else
		{
			level->SetWorldTranslation(entity, worldPosition + dir * deltaTime * 3.0f);
		}

		glm::vec3 worldRotation = level->GetWorldRotation(entity);
		worldRotation.y = std::atan2(dir.x, dir.z);
		level->SetWorldRotation(entity, worldRotation);
		return BTNodeStatus::Running;
	}
}
