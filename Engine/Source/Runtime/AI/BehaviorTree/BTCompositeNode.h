#pragma once

#include "Runtime/AI/BehaviorTree/BTNode.h"
#include "Runtime/Core/Base/Base.h"

#include <vector>

namespace HEngine
{
    class BTCompositeNode : public BTNode
    {
    public:
        virtual ~BTCompositeNode() = default;

        void AddChild(Scope<BTNode> child);

    protected:
        std::vector<Scope<BTNode>> mChildren;
    };

    class BTSelector : public BTCompositeNode
    {
    public:
        BTNodeStatus Tick(float deltaTime, Blackboard& blackboard, Entity entity) override;
        void Reset() override;

    private:
        int mCurrentIndex = 0;
    };

    class BTSequence : public BTCompositeNode
    {
    public:
        BTNodeStatus Tick(float deltaTime, Blackboard& blackboard, Entity entity) override;
        void Reset() override;

    private:
        int mCurrentIndex = 0;
    };
}
