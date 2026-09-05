#pragma once

#include "Runtime/Core/Meta/Reflection/Reflection.h"
#include "Runtime/Core/Meta/Json.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace HEngine
{
    REFLECTION_TYPE(SkeletonBoneRes)
    STRUCT(SkeletonBoneRes, Fields)
    {
        REFLECTION_BODY(SkeletonBoneRes);
        int index = 0;
        std::string name;
        int parentIndex = -1;
        glm::vec3 localBindT = glm::vec3(0.0f);
        glm::quat localBindR = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 localBindS = glm::vec3(1.0f);
    };

    REFLECTION_TYPE(SkeletonRes)
    STRUCT(SkeletonRes, Fields)
    {
        REFLECTION_BODY(SkeletonRes);
        static std::string GetExtName() { return "hskel"; }

        int version = 1;
        std::string name;
        std::vector<SkeletonBoneRes> bones;
        std::vector<std::vector<float>> inverseBindMatrices;
    };

    bool LoadSkeletonRes(const std::filesystem::path& fullPath, SkeletonRes& outRes, std::string* outError = nullptr);
    bool SaveSkeletonRes(const SkeletonRes& res, const std::filesystem::path& fullPath, std::string* outError = nullptr);
}
