#include "hepch.h"
#include "Entity.h"

namespace HEngine
{
    Entity::Entity(entt::entity handle, Scene* scene)
        : mEntityHandle(handle), mScene(scene)
    {
    }
}