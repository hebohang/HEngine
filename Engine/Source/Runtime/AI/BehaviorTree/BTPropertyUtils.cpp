#include "Hepch.h"

#include "Runtime/AI/BehaviorTree/BTPropertyUtils.h"

#include <sstream>

namespace HEngine
{
    namespace BTPropertyUtils
    {
        namespace
        {
            const BTNodePropertyRes* FindProperty(const std::vector<BTNodePropertyRes>& props, const std::string& key)
            {
                for (const BTNodePropertyRes& prop : props)
                {
                    if (prop.key == key)
                        return &prop;
                }

                return nullptr;
            }
        }

        float GetFloat(const std::vector<BTNodePropertyRes>& props, const std::string& key, float defaultVal)
        {
            const BTNodePropertyRes* prop = FindProperty(props, key);
            if (!prop)
                return defaultVal;

            try
            {
                return std::stof(prop->value);
            }
            catch (...)
            {
                return defaultVal;
            }
        }

        int GetInt(const std::vector<BTNodePropertyRes>& props, const std::string& key, int defaultVal)
        {
            const BTNodePropertyRes* prop = FindProperty(props, key);
            if (!prop)
                return defaultVal;

            try
            {
                return std::stoi(prop->value);
            }
            catch (...)
            {
                return defaultVal;
            }
        }

        bool GetBool(const std::vector<BTNodePropertyRes>& props, const std::string& key, bool defaultVal)
        {
            const BTNodePropertyRes* prop = FindProperty(props, key);
            if (!prop)
                return defaultVal;

            if (prop->value == "true" || prop->value == "1" || prop->value == "True")
                return true;
            if (prop->value == "false" || prop->value == "0" || prop->value == "False")
                return false;
            return defaultVal;
        }

        std::string GetString(const std::vector<BTNodePropertyRes>& props, const std::string& key, const std::string& defaultVal)
        {
            const BTNodePropertyRes* prop = FindProperty(props, key);
            if (!prop)
                return defaultVal;

            return prop->value;
        }

        glm::vec3 GetVec3(const std::vector<BTNodePropertyRes>& props, const std::string& key, const glm::vec3& defaultVal)
        {
            const BTNodePropertyRes* prop = FindProperty(props, key);
            if (!prop)
                return defaultVal;

            std::istringstream stream(prop->value);
            float x = defaultVal.x;
            float y = defaultVal.y;
            float z = defaultVal.z;
            char comma0 = 0;
            char comma1 = 0;

            if (!(stream >> x))
                return defaultVal;
            if (!(stream >> comma0) || comma0 != ',')
                return defaultVal;
            if (!(stream >> y))
                return defaultVal;
            if (!(stream >> comma1) || comma1 != ',')
                return defaultVal;
            if (!(stream >> z))
                return defaultVal;

            return glm::vec3(x, y, z);
        }
    }
}
