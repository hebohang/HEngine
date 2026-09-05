#include "Hepch.h"

#include "Runtime/AI/Nodes/BTCheckDistance.h"

#include "Runtime/AI/Blackboard/Blackboard.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"

#include <glm/geometric.hpp>

namespace HEngine
{
    namespace
    {
        bool CompareDistance(BTCompareOp op, float lhs, float rhs)
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

    BTNodeStatus BTCheckDistance::Tick(float deltaTime, Blackboard& blackboard, Entity entity)
    {
        (void)deltaTime;
		float dist = blackboard.GetFloat("DistToTarget", -1.0f);
		if (dist < 0.0f && entity && entity.HasComponent<TransformComponent>() && blackboard.HasKey("TargetPosition"))
		{
			const glm::vec3 selfPos = entity.GetLevel()->GetWorldTranslation(entity);
			const glm::vec3 targetPos = blackboard.GetVec3("TargetPosition", selfPos);
			const glm::vec2 delta = glm::vec2(targetPos.x - selfPos.x, targetPos.z - selfPos.z);
			dist = glm::length(delta);
            blackboard.SetFloat("DistToTarget", dist);
        }

        return CompareDistance(mOp, dist, mThreshold) ? BTNodeStatus::Success : BTNodeStatus::Failure;
    }
}
