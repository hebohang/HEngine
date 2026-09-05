#include "Hepch.h"

#include "Runtime/AI/BehaviorTree/BTBuiltinNodes.h"

#include "Runtime/AI/BehaviorTree/BTBuilder.h"
#include "Runtime/AI/BehaviorTree/BTCompositeNode.h"
#include "Runtime/AI/BehaviorTree/BTDecoratorNode.h"
#include "Runtime/AI/BehaviorTree/BTPropertyUtils.h"
#include "Runtime/AI/Nodes/BTAttack.h"
#include "Runtime/AI/Nodes/BTCheckBlackboardBool.h"
#include "Runtime/AI/Nodes/BTCheckDistance.h"
#include "Runtime/AI/Nodes/BTCheckHealth.h"
#include "Runtime/AI/Nodes/BTDeath.h"
#include "Runtime/AI/Nodes/BTFindNearestPlayer.h"
#include "Runtime/AI/Nodes/BTMoveDirectlyToTarget.h"
#include "Runtime/AI/Nodes/BTNavigateToTarget.h"
#include "Runtime/AI/Nodes/BTSetAnimParam.h"
#include "Runtime/AI/Nodes/BTWait.h"

namespace HEngine
{
    void RegisterBuiltinBTNodes()
    {
        static bool bRegistered = false;
        if (bRegistered)
            return;

        BTBuilder& builder = BTBuilder::Get();

        builder.RegisterNodeType("Selector", [](const std::vector<BTNodePropertyRes>& props)
        {
            (void)props;
            return CreateScope<BTSelector>();
        });

        builder.RegisterNodeType("Sequence", [](const std::vector<BTNodePropertyRes>& props)
        {
            (void)props;
            return CreateScope<BTSequence>();
        });

        builder.RegisterNodeType("Inverter", [](const std::vector<BTNodePropertyRes>& props)
        {
            (void)props;
            return CreateScope<BTInverter>();
        });

        builder.RegisterNodeType("Repeat", [](const std::vector<BTNodePropertyRes>& props)
        {
            const int maxCount = BTPropertyUtils::GetInt(props, "maxCount", -1);
            return CreateScope<BTRepeat>(maxCount);
        });

        builder.RegisterNodeType("AlwaysSucceed", [](const std::vector<BTNodePropertyRes>& props)
        {
            (void)props;
            return CreateScope<BTAlwaysSucceed>();
        });

        builder.RegisterNodeType("FindNearestPlayer", [](const std::vector<BTNodePropertyRes>& props)
        {
            (void)props;
            return CreateScope<BTFindNearestPlayer>();
        });

        builder.RegisterNodeType("MoveDirectlyToTarget", [](const std::vector<BTNodePropertyRes>& props)
        {
            const float arriveThreshold = BTPropertyUtils::GetFloat(props, "arriveThreshold", 1.5f);
            return CreateScope<BTMoveDirectlyToTarget>(arriveThreshold);
        });

        builder.RegisterNodeType("NavigateToTarget", [](const std::vector<BTNodePropertyRes>& props)
        {
            const float arriveThreshold = BTPropertyUtils::GetFloat(props, "arriveThreshold", 1.5f);
            const float repathInterval = BTPropertyUtils::GetFloat(props, "repathInterval", 1.0f);
            return CreateScope<BTNavigateToTarget>(arriveThreshold, repathInterval);
        });

        builder.RegisterNodeType("Attack", [](const std::vector<BTNodePropertyRes>& props)
        {
            const float cooldown = BTPropertyUtils::GetFloat(props, "cooldown", 1.5f);
            const float damage = BTPropertyUtils::GetFloat(props, "damage", 10.0f);
            const std::string attackTrigger = BTPropertyUtils::GetString(props, "attackTrigger", "bAttacking");
            return CreateScope<BTAttack>(cooldown, damage, attackTrigger);
        });

        builder.RegisterNodeType("SetAnimParam", [](const std::vector<BTNodePropertyRes>& props)
        {
            const std::string mode = BTPropertyUtils::GetString(props, "mode", "Bool");
            const std::string paramName = BTPropertyUtils::GetString(props, "paramName", "");
            if (mode == "Bool")
            {
                const bool value = BTPropertyUtils::GetBool(props, "value", false);
                return CreateScope<BTSetAnimParam>(paramName, value);
            }

            if (mode == "Int")
            {
                const int value = BTPropertyUtils::GetInt(props, "value", 0);
                return CreateScope<BTSetAnimParam>(paramName, value);
            }

            if (mode == "Float")
            {
                const float value = BTPropertyUtils::GetFloat(props, "value", 0.0f);
                return CreateScope<BTSetAnimParam>(paramName, value);
            }

            return CreateScope<BTSetAnimParam>(paramName);
        });

        builder.RegisterNodeType("CheckDistance", [](const std::vector<BTNodePropertyRes>& props)
        {
            const std::string opStr = BTPropertyUtils::GetString(props, "op", "Less");
            BTCompareOp op = BTCompareOp::Less;
            if (opStr == "Greater")
                op = BTCompareOp::Greater;
            else if (opStr == "LessEqual")
                op = BTCompareOp::LessEqual;
            else if (opStr == "GreaterEqual")
                op = BTCompareOp::GreaterEqual;

            const float threshold = BTPropertyUtils::GetFloat(props, "threshold", 0.0f);
            return CreateScope<BTCheckDistance>(op, threshold);
        });

        builder.RegisterNodeType("CheckHealth", [](const std::vector<BTNodePropertyRes>& props)
        {
            const std::string opStr = BTPropertyUtils::GetString(props, "op", "LessEqual");
            BTCompareOp op = BTCompareOp::LessEqual;
            if (opStr == "Less")
                op = BTCompareOp::Less;
            else if (opStr == "Greater")
                op = BTCompareOp::Greater;
            else if (opStr == "GreaterEqual")
                op = BTCompareOp::GreaterEqual;

            const float threshold = BTPropertyUtils::GetFloat(props, "threshold", 0.0f);
            return CreateScope<BTCheckHealth>(threshold, op);
        });

        builder.RegisterNodeType("CheckBlackboardBool", [](const std::vector<BTNodePropertyRes>& props)
        {
            const std::string key = BTPropertyUtils::GetString(props, "key", "");
            const bool expectedValue = BTPropertyUtils::GetBool(props, "expectedValue", true);
            return CreateScope<BTCheckBlackboardBool>(key, expectedValue);
        });

        builder.RegisterNodeType("Wait", [](const std::vector<BTNodePropertyRes>& props)
        {
            const float waitTime = BTPropertyUtils::GetFloat(props, "waitTime", 0.0f);
            return CreateScope<BTWait>(waitTime);
        });

        builder.RegisterNodeType("Death", [](const std::vector<BTNodePropertyRes>& props)
        {
            const std::string deathParam = BTPropertyUtils::GetString(props, "deathParam", "bDead");
            return CreateScope<BTDeath>(deathParam);
        });

        bRegistered = true;
    }
}
