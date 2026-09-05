#pragma once

#include "Runtime/Core/Meta/Reflection/Reflection.h"
#include "Runtime/Core/Meta/Json.h"

#include <filesystem>
#include <string>
#include <vector>

namespace HEngine
{
    enum class AnimControllerParamType
    {
        Bool = 0,
        Int,
        Float,
        Trigger
    };

    inline void to_json(Json& json, const AnimControllerParamType& type)
    {
        switch (type)
        {
        case AnimControllerParamType::Bool:
            json = "Bool";
            break;
        case AnimControllerParamType::Int:
            json = "Int";
            break;
        case AnimControllerParamType::Float:
            json = "Float";
            break;
        case AnimControllerParamType::Trigger:
            json = "Trigger";
            break;
        default:
            json = "Float";
            break;
        }
    }

    inline void from_json(const Json& json, AnimControllerParamType& type)
    {
        if (json.is_number_integer())
        {
            type = static_cast<AnimControllerParamType>(json.get<int>());
            return;
        }

        const std::string typeName = json.get<std::string>();
        if (typeName == "Bool")
            type = AnimControllerParamType::Bool;
        else if (typeName == "Int")
            type = AnimControllerParamType::Int;
        else if (typeName == "Float")
            type = AnimControllerParamType::Float;
        else if (typeName == "Trigger")
            type = AnimControllerParamType::Trigger;
        else
            type = AnimControllerParamType::Float;
    }

    REFLECTION_TYPE(AnimControllerParameterRes)
    STRUCT(AnimControllerParameterRes, Fields)
    {
        REFLECTION_BODY(AnimControllerParameterRes);
        std::string name;
        AnimControllerParamType type = AnimControllerParamType::Float;
        Json defaultValue = 0.0f;
    };

    REFLECTION_TYPE(AnimControllerNodeRes)
    STRUCT(AnimControllerNodeRes, Fields)
    {
        REFLECTION_BODY(AnimControllerNodeRes);
        int id = 0;
        std::string type = "State";
        std::string name;
        std::vector<float> position = { 0.0f, 0.0f };
        std::string clipPath;
    };

    REFLECTION_TYPE(AnimControllerConditionRes)
    STRUCT(AnimControllerConditionRes, Fields)
    {
        REFLECTION_BODY(AnimControllerConditionRes);
        std::string param;
        std::string op = "==";
        Json value = true;
    };

    REFLECTION_TYPE(AnimControllerTransitionRes)
    STRUCT(AnimControllerTransitionRes, Fields)
    {
        REFLECTION_BODY(AnimControllerTransitionRes);
        int fromNodeId = 0;
        int toNodeId = 0;
        float duration = 0.1f;
        std::vector<AnimControllerConditionRes> conditions;
    };

    REFLECTION_TYPE(AnimControllerRes)
    STRUCT(AnimControllerRes, Fields)
    {
        REFLECTION_BODY(AnimControllerRes);
        static std::string GetExtName() { return "hanimctrl"; }

        int version = 1;
        std::string name;
        std::string targetSkeletonPath;
        std::vector<AnimControllerParameterRes> parameters;
        std::vector<AnimControllerNodeRes> nodes;
        std::vector<AnimControllerTransitionRes> transitions;
        int entryNodeId = 0;
    };

    bool LoadAnimControllerRes(const std::filesystem::path& fullPath, AnimControllerRes& outRes, std::string* outError = nullptr);
    bool SaveAnimControllerRes(const AnimControllerRes& res, const std::filesystem::path& fullPath, std::string* outError = nullptr);
}
