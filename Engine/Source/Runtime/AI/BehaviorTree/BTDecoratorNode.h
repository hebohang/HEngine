#pragma once

#include "Runtime/AI/BehaviorTree/BTNode.h"
#include "Runtime/Core/Base/Base.h"

namespace HEngine
{
    class BTDecoratorNode : public BTNode
    {
    public:
        virtual ~BTDecoratorNode() = default;

        void SetChild(Scope<BTNode> child);

    protected:
        Scope<BTNode> mChild;
    };

    class BTInverter : public BTDecoratorNode
    {
    public:
        BTNodeStatus Tick(float deltaTime, Blackboard& blackboard, Entity entity) override;
        void Reset() override;
    };

    class BTRepeat : public BTDecoratorNode
    {
    public:
        explicit BTRepeat(int maxCount = -1)
            : mMaxCount(maxCount)
        {
        }

        BTNodeStatus Tick(float deltaTime, Blackboard& blackboard, Entity entity) override;
        void Reset() override;

    private:
        int mMaxCount = -1;
        int mCurrentCount = 0;
    };

    class BTAlwaysSucceed : public BTDecoratorNode
    {
    public:
        BTNodeStatus Tick(float deltaTime, Blackboard& blackboard, Entity entity) override;
        void Reset() override;
    };
}
