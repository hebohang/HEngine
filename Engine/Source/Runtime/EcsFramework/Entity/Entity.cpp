#include "hepch.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"

namespace HEngine
{
    Entity::Entity(entt::entity handle, Level* level)
        : mEntityHandle(handle), mLevel(level)
    {
        //AddComponent<IDComponent>(UUID());
    }

    UUID Entity::GetUUID() { return GetComponent<IDComponent>().ID; }
    const std::string& Entity::GetName() { return GetComponent<TagComponent>().Tag; }
}