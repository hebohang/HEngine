#pragma once

#include "Runtime/AI/BehaviorTree/BTNode.h"
#include "Runtime/AI/Blackboard/Blackboard.h"
#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <string>

namespace HEngine
{
    REFLECTION_TYPE(BehaviorTreeComponent)
    CLASS(BehaviorTreeComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(BehaviorTreeComponent);
    public:
        BehaviorTreeComponent() = default;
        BehaviorTreeComponent(BehaviorTreeComponent&&) noexcept = default;
        BehaviorTreeComponent& operator=(BehaviorTreeComponent&&) noexcept = default;
        BehaviorTreeComponent(const BehaviorTreeComponent& other)
            : btResPath(other.btResPath),
            tickInterval(other.tickInterval),
            bLoaded(false),
            mRootNode(nullptr),
            mBlackboard(other.mBlackboard),
            bEnabled(other.bEnabled),
            mTickTimer(0.0f)
        {
        }

        BehaviorTreeComponent& operator=(const BehaviorTreeComponent& other)
        {
            if (this == &other)
                return *this;

            ComponentBase::operator=(other);
            btResPath = other.btResPath;
            tickInterval = other.tickInterval;
            bLoaded = false;
            mRootNode.reset();
            mBlackboard = other.mBlackboard;
            bEnabled = other.bEnabled;
            mTickTimer = 0.0f;
            return *this;
        }

        std::string btResPath = "None";
        float tickInterval = 0.0f;

        PROPERTY(Disable)
        bool bLoaded = false;

        PROPERTY(Disable)
        Scope<BTNode> mRootNode = nullptr;

        PROPERTY(Disable)
        Blackboard mBlackboard;

        PROPERTY(Disable)
        bool bEnabled = true;

        PROPERTY(Disable)
        float mTickTimer = 0.0f;
    };
    REGISTER_COMPONENT_TYPE(BehaviorTreeComponent)
}
