#include "Hepch.h"

#include "Runtime/AI/Nodes/BTNavigateToTarget.h"

#include "Runtime/AI/Blackboard/Blackboard.h"
#include "Runtime/AI/Navigation/NavigationService.h"
#include "Runtime/EcsFramework/Component/Character/CharacterMovementComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"

#include <glm/geometric.hpp>

#include <cmath>

namespace HEngine
{
    BTNodeStatus BTNavigateToTarget::Tick(float deltaTime, Blackboard& blackboard, Entity entity)
    {
		if (!entity || !entity.HasComponent<TransformComponent>())
			return BTNodeStatus::Failure;

		Level* level = entity.GetLevel();
		const glm::vec3 worldPosition = level->GetWorldTranslation(entity);
		const glm::vec3 finalTarget = blackboard.GetVec3("TargetPosition", worldPosition);
		glm::vec3 finalOffset = finalTarget - worldPosition;
		finalOffset.y = 0.0f;
		const float finalDist = glm::length(finalOffset);
        blackboard.SetFloat("DistToTarget", finalDist);

        if (finalDist < mArriveThreshold)
        {
            if (entity.HasComponent<CharacterMovementComponent>())
            {
                auto& movement = entity.GetComponent<CharacterMovementComponent>();
                movement.RuntimeDesiredVelocity = glm::vec3(0.0f);
                movement.RuntimeHasExternalVelocity = false;
            }

            mPath.clear();
            mPathIndex = 0;
            mRepathTimer = 0.0f;
            return BTNodeStatus::Success;
        }

		mRepathTimer -= deltaTime;
		if (mRepathTimer <= 0.0f || mPath.empty() || mPathIndex >= static_cast<int>(mPath.size()))
		{
			if (!NavigationService::Get().FindPath(worldPosition, finalTarget, mPath))
				return BTNodeStatus::Failure;

            mPathIndex = 0;
            mRepathTimer = mRepathInterval;
        }

		if (mPathIndex >= static_cast<int>(mPath.size()))
			return BTNodeStatus::Failure;

		glm::vec3 waypoint = mPath[static_cast<size_t>(mPathIndex)];
		glm::vec3 toWaypoint = waypoint - worldPosition;
		toWaypoint.y = 0.0f;

        if (glm::length(toWaypoint) < 0.5f)
        {
            ++mPathIndex;
			if (mPathIndex >= static_cast<int>(mPath.size()))
				return BTNodeStatus::Running;

			waypoint = mPath[static_cast<size_t>(mPathIndex)];
			toWaypoint = waypoint - worldPosition;
			toWaypoint.y = 0.0f;
		}

        glm::vec3 dir = glm::length2(toWaypoint) > 0.0001f ? glm::normalize(toWaypoint) : glm::vec3(0.0f);
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

    void BTNavigateToTarget::Reset()
    {
        mPath.clear();
        mPathIndex = 0;
        mRepathTimer = 0.0f;
    }
}
