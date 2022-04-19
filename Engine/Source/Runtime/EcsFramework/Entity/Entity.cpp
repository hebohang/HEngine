#include "hepch.h"
#include "Runtime/EcsFramework/Entity/Entity.h"

namespace HEngine
{
    Entity::Entity(entt::entity handle, Level* level)
        : mEntityHandle(handle), mLevel(level)
    {
        //AddComponent<IDComponent>(UUID());
    }
}