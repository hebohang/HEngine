#include "Serializer.h"

namespace HEngine
{
    template<>
    void Serializer::Serialize(Json& json, const Json& obj)
    {
        json = std::move(obj);
    }

    template<>
    void Serializer::Deserialize(const Json& json, Json& obj)
    {
        obj = std::move(json);
    }
}
