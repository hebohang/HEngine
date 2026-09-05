#include "Hepch.h"

#include "Runtime/AI/Blackboard/Blackboard.h"

namespace HEngine
{
    void Blackboard::SetFloat(const std::string& key, float value)
    {
        mData[key] = value;
    }

    float Blackboard::GetFloat(const std::string& key, float defaultVal) const
    {
        auto iter = mData.find(key);
        if (iter == mData.end())
            return defaultVal;

        if (const float* value = std::get_if<float>(&iter->second))
            return *value;

        return defaultVal;
    }

    void Blackboard::SetInt(const std::string& key, int value)
    {
        mData[key] = value;
    }

    int Blackboard::GetInt(const std::string& key, int defaultVal) const
    {
        auto iter = mData.find(key);
        if (iter == mData.end())
            return defaultVal;

        if (const int* value = std::get_if<int>(&iter->second))
            return *value;

        return defaultVal;
    }

    void Blackboard::SetBool(const std::string& key, bool value)
    {
        mData[key] = value;
    }

    bool Blackboard::GetBool(const std::string& key, bool defaultVal) const
    {
        auto iter = mData.find(key);
        if (iter == mData.end())
            return defaultVal;

        if (const bool* value = std::get_if<bool>(&iter->second))
            return *value;

        return defaultVal;
    }

    void Blackboard::SetVec3(const std::string& key, const glm::vec3& value)
    {
        mData[key] = value;
    }

    glm::vec3 Blackboard::GetVec3(const std::string& key, const glm::vec3& defaultVal) const
    {
        auto iter = mData.find(key);
        if (iter == mData.end())
            return defaultVal;

        if (const glm::vec3* value = std::get_if<glm::vec3>(&iter->second))
            return *value;

        return defaultVal;
    }

    void Blackboard::SetEntityHandle(const std::string& key, uint32_t value)
    {
        mData[key] = value;
    }

    uint32_t Blackboard::GetEntityHandle(const std::string& key, uint32_t defaultVal) const
    {
        auto iter = mData.find(key);
        if (iter == mData.end())
            return defaultVal;

        if (const uint32_t* value = std::get_if<uint32_t>(&iter->second))
            return *value;

        return defaultVal;
    }

    bool Blackboard::HasKey(const std::string& key) const
    {
        return mData.find(key) != mData.end();
    }

    void Blackboard::RemoveKey(const std::string& key)
    {
        mData.erase(key);
    }

    void Blackboard::Clear()
    {
        mData.clear();
    }

    std::vector<std::string> Blackboard::GetAllKeys() const
    {
        std::vector<std::string> keys;
        keys.reserve(mData.size());
        for (const auto& [key, value] : mData)
        {
            (void)value;
            keys.push_back(key);
        }

        return keys;
    }
}
