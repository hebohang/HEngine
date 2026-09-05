#pragma once

#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Core/Meta/Reflection/Reflection.h"

#include <filesystem>
#include <string>
#include <vector>

namespace HEngine
{
    REFLECTION_TYPE(BTNodePropertyRes)
    STRUCT(BTNodePropertyRes, Fields)
    {
        REFLECTION_BODY(BTNodePropertyRes);
        std::string key;
        std::string type = "float";
        std::string value;
    };

    REFLECTION_TYPE(BTNodeRes)
    STRUCT(BTNodeRes, Fields)
    {
        REFLECTION_BODY(BTNodeRes);
        int id = 0;
        std::string type;
        std::string name;
        int parentId = -1;
        int childOrder = 0;
        std::vector<BTNodePropertyRes> properties;
    };

    REFLECTION_TYPE(BTBlackboardKeyRes)
    STRUCT(BTBlackboardKeyRes, Fields)
    {
        REFLECTION_BODY(BTBlackboardKeyRes);
        std::string key;
        std::string type = "float";
        std::string defaultValue;
    };

    REFLECTION_TYPE(BehaviorTreeRes)
    STRUCT(BehaviorTreeRes, Fields)
    {
        REFLECTION_BODY(BehaviorTreeRes);
        static std::string GetExtName() { return "hbt"; }

        int version = 1;
        std::string name;
        int rootNodeId = 0;
        std::vector<BTNodeRes> nodes;
        std::vector<BTBlackboardKeyRes> blackboardKeys;
    };

    bool LoadBehaviorTreeRes(const std::filesystem::path& fullPath, BehaviorTreeRes& outRes, std::string* outError = nullptr);
    bool SaveBehaviorTreeRes(const BehaviorTreeRes& res, const std::filesystem::path& fullPath, std::string* outError = nullptr);
}
