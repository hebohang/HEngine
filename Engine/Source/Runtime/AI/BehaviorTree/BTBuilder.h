#pragma once

#include "Runtime/AI/BehaviorTree/BTNode.h"
#include "Runtime/AI/Blackboard/Blackboard.h"
#include "Runtime/Core/Base/Base.h"
#include "Runtime/Resource/ResType/BehaviorTreeRes.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace HEngine
{
    class BTBuilder
    {
    public:
        using NodeFactory = std::function<Scope<BTNode>(const std::vector<BTNodePropertyRes>&)>;

        static BTBuilder& Get();

        void RegisterNodeType(const std::string& typeName, NodeFactory factory);
        Scope<BTNode> Build(const BehaviorTreeRes& res) const;
        void InitBlackboard(const BehaviorTreeRes& res, Blackboard& bb) const;

    private:
        std::unordered_map<std::string, NodeFactory> mFactories;
    };
}
