#pragma once

#include <entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace HEngine
{
    class Blackboard
    {
    public:
        using ValueType = std::variant<float, int, bool, glm::vec3, uint32_t>;

        void SetFloat(const std::string& key, float value);
        float GetFloat(const std::string& key, float defaultVal = 0.0f) const;

        void SetInt(const std::string& key, int value);
        int GetInt(const std::string& key, int defaultVal = 0) const;

        void SetBool(const std::string& key, bool value);
        bool GetBool(const std::string& key, bool defaultVal = false) const;

        void SetVec3(const std::string& key, const glm::vec3& value);
        glm::vec3 GetVec3(const std::string& key, const glm::vec3& defaultVal = glm::vec3(0.0f)) const;

        void SetEntityHandle(const std::string& key, uint32_t value);
        uint32_t GetEntityHandle(const std::string& key, uint32_t defaultVal = static_cast<uint32_t>(entt::null)) const;

        [[nodiscard]] bool HasKey(const std::string& key) const;
        void RemoveKey(const std::string& key);
        void Clear();
        [[nodiscard]] std::vector<std::string> GetAllKeys() const;

    private:
        std::unordered_map<std::string, ValueType> mData;
    };
}
