#pragma once

#include "Runtime/AI/BehaviorTree/BTActionNode.h"

#include <string>
#include <variant>

namespace HEngine
{
    class BTSetAnimParam : public BTActionNode
    {
    public:
        enum class ParamMode
        {
            Bool = 0,
            Int,
            Float,
            Trigger
        };

        BTSetAnimParam(const std::string& paramName, bool value);
        BTSetAnimParam(const std::string& paramName, int value);
        BTSetAnimParam(const std::string& paramName, float value);
        explicit BTSetAnimParam(const std::string& paramName);

        BTNodeStatus Tick(float deltaTime, Blackboard& blackboard, Entity entity) override;

    private:
        ParamMode mMode = ParamMode::Bool;
        std::string mParamName;
        std::variant<bool, int, float> mValue = false;
    };
}
