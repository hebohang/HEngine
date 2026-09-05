#pragma once
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// TODO: Change GetName func within reflection code
#define HE_SERIALIZER(Type, ...)  \
    inline void to_json(nlohmann::json& nlohmann_json_j, const Type& nlohmann_json_t) { NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, __VA_ARGS__)) } \
    inline void from_json(const nlohmann::json& nlohmann_json_j, Type& nlohmann_json_t) { Type nlohmann_json_default_obj; NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_FROM_WITH_DEFAULT, __VA_ARGS__)) } \
    inline const char* GetName(Type type) { return #Type; }
#define HE_SERIALIZER_WITH_BASE(Type, BASE_TYPE, ...)  \
    inline void to_json(nlohmann::json& nlohmann_json_j, const Type& nlohmann_json_t) { nlohmann::to_json(nlohmann_json_j, static_cast<BASE_TYPE>(nlohmann_json_t)); NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, __VA_ARGS__)) } \
    inline void from_json(const nlohmann::json& nlohmann_json_j, Type& nlohmann_json_t) { Type nlohmann_json_default_obj; nlohmann::from_json(nlohmann_json_j, static_cast<BASE_TYPE&>(nlohmann_json_t)); NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_FROM_WITH_DEFAULT, __VA_ARGS__)) }

namespace HEngine
{
    using Json = nlohmann::json;
}

namespace glm
{
    inline void to_json(nlohmann::json& j, const glm::vec2& p)
    {
        j = { p.x, p.y };
    }

    inline void from_json(const nlohmann::json& j, glm::vec2& p)
    {
        p = { j[0], j[1] };
    }

    inline void to_json(nlohmann::json& j, const glm::vec3& p)
    {
        j = { p.x, p.y, p.z };
    }

    inline void from_json(const nlohmann::json& j, glm::vec3& p)
    {
        p = { j[0], j[1], j[2] };
    }

    inline void to_json(nlohmann::json& j, const glm::vec4& p)
    {
        j = { p.x, p.y, p.z, p.w };
    }

    inline void from_json(const nlohmann::json& j, glm::vec4& p)
    {
        p = { j[0], j[1], j[2], j[3] };
    }

    inline void to_json(nlohmann::json& j, const glm::quat& q)
    {
        j = { q.w, q.x, q.y, q.z };
    }

    inline void from_json(const nlohmann::json& j, glm::quat& q)
    {
        q = { j[0], j[1], j[2], j[3] };
    }
}
