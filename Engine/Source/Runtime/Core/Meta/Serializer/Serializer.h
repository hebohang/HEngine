#pragma once
#include "Runtime/Core/Meta/Json.h"

namespace HEngine
{
    class Serializer
    {
    public:
        template<typename T>
        static void Serialize(Json& json, const T& obj) 
        {
            nlohmann::to_json(json, obj);
        }

        template<typename T>
        static void Deserialize(const Json& json, T& obj)
        {
            nlohmann::from_json(json, obj);
        }
    };

    template<>
    void Serializer::Serialize(Json& json, const Json& obj);

    template<>
    void Serializer::Deserialize(const Json& json, Json& obj);
}
