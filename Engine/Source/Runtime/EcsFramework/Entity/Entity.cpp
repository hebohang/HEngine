#include "Hepch.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Component/Basic/IDComponent.h"
#include "Runtime/EcsFramework/Component/Basic/TagComponent.h"

namespace HEngine
{
    Entity::Entity(EntityHandle handle, Level* level)
        : mEntityHandle(handle), mLevel(level)
    {
        if (level) mLevelLifetime = level->mEntityLifetime;
    }

    UUID Entity::UUID() { return GetComponent<IDComponent>().ID; }
    const std::string& Entity::GetName() { return GetComponent<TagComponent>().Tag; }
}
