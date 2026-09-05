#pragma once

#include "Runtime/Resource/ResType/BehaviorTreeRes.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace HEngine
{
    namespace BTPropertyUtils
    {
        float GetFloat(const std::vector<BTNodePropertyRes>& props, const std::string& key, float defaultVal);
        int GetInt(const std::vector<BTNodePropertyRes>& props, const std::string& key, int defaultVal);
        bool GetBool(const std::vector<BTNodePropertyRes>& props, const std::string& key, bool defaultVal);
        std::string GetString(const std::vector<BTNodePropertyRes>& props, const std::string& key, const std::string& defaultVal);
        glm::vec3 GetVec3(const std::vector<BTNodePropertyRes>& props, const std::string& key, const glm::vec3& defaultVal);
    }
}
