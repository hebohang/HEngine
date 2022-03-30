#include "hepch.h"
#include "Entity.h"

namespace HEngine
{
    Entity::Entity(entt::entity handle, Scene* scene)
        : m_EntityHandle(handle), m_Scene(scene)
    {
    }
}