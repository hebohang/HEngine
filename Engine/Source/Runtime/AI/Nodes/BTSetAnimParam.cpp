#include "Hepch.h"

#include "Runtime/AI/Nodes/BTSetAnimParam.h"

#include "Runtime/AI/Blackboard/Blackboard.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"

namespace HEngine
{
    BTSetAnimParam::BTSetAnimParam(const std::string& paramName, bool value)
        : mMode(ParamMode::Bool), mParamName(paramName), mValue(value)
    {
    }

    BTSetAnimParam::BTSetAnimParam(const std::string& paramName, int value)
        : mMode(ParamMode::Int), mParamName(paramName), mValue(value)
    {
    }

    BTSetAnimParam::BTSetAnimParam(const std::string& paramName, float value)
        : mMode(ParamMode::Float), mParamName(paramName), mValue(value)
    {
    }

    BTSetAnimParam::BTSetAnimParam(const std::string& paramName)
        : mMode(ParamMode::Trigger), mParamName(paramName), mValue(false)
    {
    }

    BTNodeStatus BTSetAnimParam::Tick(float deltaTime, Blackboard& blackboard, Entity entity)
    {
        (void)deltaTime;
        (void)blackboard;
        if (!entity || !entity.HasComponent<AnimatorComponent>())
            return BTNodeStatus::Failure;

        auto& animator = entity.GetComponent<AnimatorComponent>();
        switch (mMode)
        {
        case ParamMode::Bool:
            animator.SetBool(mParamName, std::get<bool>(mValue));
            break;
        case ParamMode::Int:
            animator.SetInt(mParamName, std::get<int>(mValue));
            break;
        case ParamMode::Float:
            animator.SetFloat(mParamName, std::get<float>(mValue));
            break;
        case ParamMode::Trigger:
            animator.SetTrigger(mParamName);
            break;
        default:
            return BTNodeStatus::Failure;
        }

        return BTNodeStatus::Success;
    }
}
