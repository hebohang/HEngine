#include "Hepch.h"

#include "Runtime/AI/Nodes/BTFindNearestPlayer.h"

#include "Runtime/AI/Blackboard/Blackboard.h"
#include "Runtime/EcsFramework/Component/Basic/TagComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Level/Level.h"

#include <glm/geometric.hpp>

#include <cmath>
#include <cfloat>

namespace HEngine
{
    BTNodeStatus BTFindNearestPlayer::Tick(float deltaTime, Blackboard& blackboard, Entity entity)
    {
        (void)deltaTime;
        if (!entity)
            return BTNodeStatus::Failure;

		Level* level = entity.GetLevel();
		if (!level || !entity.HasComponent<TransformComponent>())
			return BTNodeStatus::Failure;

		const glm::vec3 selfPos = level->GetWorldTranslation(entity);
        float minDist2 = FLT_MAX;
        EntityHandle nearestHandle = entt::null;
        glm::vec3 nearestPos(0.0f);

        auto view = level->mRegistry.view<TagComponent, TransformComponent>();
        for (auto handle : view)
        {
            const auto& tag = view.get<TagComponent>(handle);
			if (tag.Tag != "Player")
				continue;

			const glm::vec3 targetPos = level->GetWorldTranslation(Entity{ handle, level });
			const glm::vec2 offset = glm::vec2(targetPos.x - selfPos.x, targetPos.z - selfPos.z);
            const float dist2 = glm::dot(offset, offset);
            if (dist2 < minDist2)
            {
                minDist2 = dist2;
                nearestHandle = handle;
                nearestPos = targetPos;
            }
        }

        if (nearestHandle == entt::null)
            return BTNodeStatus::Failure;

        blackboard.SetVec3("TargetPosition", nearestPos);
        blackboard.SetEntityHandle("TargetEntityHandle", static_cast<uint32_t>(nearestHandle));
        blackboard.SetFloat("DistToTarget", std::sqrt(minDist2));
        return BTNodeStatus::Success;
    }
}
