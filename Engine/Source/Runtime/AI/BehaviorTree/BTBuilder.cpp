#include "Hepch.h"

#include "Runtime/AI/BehaviorTree/BTBuilder.h"
#include "Runtime/AI/BehaviorTree/BTCompositeNode.h"
#include "Runtime/AI/BehaviorTree/BTDecoratorNode.h"

#include <algorithm>
#include <sstream>

namespace HEngine
{
    BTBuilder& BTBuilder::Get()
    {
        static BTBuilder sInstance;
        return sInstance;
    }

    void BTBuilder::RegisterNodeType(const std::string& typeName, NodeFactory factory)
    {
        if (typeName.empty() || !factory)
            return;

        mFactories[typeName] = std::move(factory);
    }

    Scope<BTNode> BTBuilder::Build(const BehaviorTreeRes& res) const
    {
        std::unordered_map<int, Scope<BTNode>> createdNodes;
        std::unordered_map<int, std::vector<BTNodeRes>> childNodes;

        for (const BTNodeRes& nodeRes : res.nodes)
        {
            auto factoryIter = mFactories.find(nodeRes.type);
            if (factoryIter == mFactories.end())
            {
                HE_CORE_WARN("BTBuilder: unknown node type '{0}'", nodeRes.type);
                continue;
            }

            Scope<BTNode> node = factoryIter->second(nodeRes.properties);
            if (!node)
                continue;

            node->mName = nodeRes.name.empty() ? nodeRes.type : nodeRes.name;
            createdNodes[nodeRes.id] = std::move(node);
            childNodes[nodeRes.parentId].push_back(nodeRes);
        }

        std::function<Scope<BTNode>(int)> buildNode = [&](int nodeId) -> Scope<BTNode>
        {
            auto nodeIter = createdNodes.find(nodeId);
            if (nodeIter == createdNodes.end() || !nodeIter->second)
                return nullptr;

            Scope<BTNode> node = std::move(nodeIter->second);

            auto childIter = childNodes.find(nodeId);
            if (childIter == childNodes.end())
                return node;

            std::vector<BTNodeRes> children = childIter->second;
            std::sort(children.begin(), children.end(), [](const BTNodeRes& a, const BTNodeRes& b)
            {
                return a.childOrder < b.childOrder;
            });

            if (BTCompositeNode* composite = dynamic_cast<BTCompositeNode*>(node.get()))
            {
                for (const BTNodeRes& childRes : children)
                {
                    Scope<BTNode> child = buildNode(childRes.id);
                    if (child)
                        composite->AddChild(std::move(child));
                }
                return node;
            }

            if (BTDecoratorNode* decorator = dynamic_cast<BTDecoratorNode*>(node.get()))
            {
                for (const BTNodeRes& childRes : children)
                {
                    Scope<BTNode> child = buildNode(childRes.id);
                    if (child)
                    {
                        decorator->SetChild(std::move(child));
                        break;
                    }
                }
            }

            return node;
        };

        return buildNode(res.rootNodeId);
    }

    void BTBuilder::InitBlackboard(const BehaviorTreeRes& res, Blackboard& bb) const
    {
        for (const BTBlackboardKeyRes& entry : res.blackboardKeys)
        {
            if (bb.HasKey(entry.key))
                continue;

            if (entry.type == "float")
            {
                try
                {
                    bb.SetFloat(entry.key, std::stof(entry.defaultValue));
                }
                catch (...)
                {
                    bb.SetFloat(entry.key, 0.0f);
                }
            }
            else if (entry.type == "int")
            {
                try
                {
                    bb.SetInt(entry.key, std::stoi(entry.defaultValue));
                }
                catch (...)
                {
                    bb.SetInt(entry.key, 0);
                }
            }
            else if (entry.type == "bool")
            {
                const bool bValue = entry.defaultValue == "true" || entry.defaultValue == "1" || entry.defaultValue == "True";
                bb.SetBool(entry.key, bValue);
            }
            else if (entry.type == "vec3")
            {
                float x = 0.0f;
                float y = 0.0f;
                float z = 0.0f;
                char comma0 = 0;
                char comma1 = 0;
                std::stringstream stream(entry.defaultValue);
                if ((stream >> x) && (stream >> comma0) && comma0 == ',' && (stream >> y) && (stream >> comma1) && comma1 == ',' && (stream >> z))
                    bb.SetVec3(entry.key, glm::vec3(x, y, z));
                else
                    bb.SetVec3(entry.key, glm::vec3(0.0f));
            }
        }
    }
}
