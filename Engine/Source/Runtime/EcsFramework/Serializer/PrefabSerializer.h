#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Core/Meta/Json.h"
#include "Runtime/EcsFramework/Level/Level.h"

#include <glm/glm.hpp>

#include <string>

namespace HEngine
{
    class PrefabSerializer
    {
    public:
        static void SavePrefab(Entity entity, const std::string& filepath);
        static Entity InstantiatePrefab(Level* level, const std::string& prefabPath);
        static Entity InstantiatePrefab(Level* level, const std::string& prefabPath, const glm::vec3& position);
        static Entity InstantiatePrefab(Level* level, const std::string& prefabPath, const glm::vec3& position, const glm::vec3& rotation);

    private:
        static Entity InstantiateFromJson(Level* level, const Json& prefabJson);
    };
}
