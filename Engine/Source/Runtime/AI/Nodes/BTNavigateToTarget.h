#pragma once

#include "Runtime/AI/BehaviorTree/BTActionNode.h"

#include <glm/glm.hpp>

#include <vector>

namespace HEngine
{
    class BTNavigateToTarget : public BTActionNode
    {
    public:
        BTNavigateToTarget(float arriveThreshold = 1.5f, float repathInterval = 1.0f)
            : mArriveThreshold(arriveThreshold), mRepathInterval(repathInterval)
        {
        }

        BTNodeStatus Tick(float deltaTime, Blackboard& blackboard, Entity entity) override;
        void Reset() override;

    private:
        float mArriveThreshold = 1.5f;
        float mRepathInterval = 1.0f;
        std::vector<glm::vec3> mPath;
        int mPathIndex = 0;
        float mRepathTimer = 0.0f;
    };
}
