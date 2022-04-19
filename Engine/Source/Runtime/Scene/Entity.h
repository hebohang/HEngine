#pragma once

#include "Runtime/Core/UUID.h"
#include "Scene.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "entt.hpp"


namespace HEngine
{
    class Entity
    {
    public:
        Entity() = default;
        Entity(entt::entity handle, Scene* scene);
        Entity(const Entity& other) = default;

        template<typename T, typename... Args>
        T& AddComponent(Args&&... args)
        {
            HE_CORE_ASSERT(!HasComponent<T>(), "Entity already has component!");
            T& component = mScene->mRegistry.emplace<T>(mEntityHandle, std::forward<Args>(args)...);
            mScene->OnComponentAdded<T>(*this, component);
            return component;
        }

		template<typename T, typename... Args>
		T& AddOrReplaceComponent(Args&&... args)
		{
			T& component = mScene->mRegistry.emplace_or_replace<T>(mEntityHandle, std::forward<Args>(args)...);
			mScene->OnComponentAdded<T>(*this, component);
			return component;
		}

        template<typename T>
        T& GetComponent()
        {
            HE_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!");
            return mScene->mRegistry.get<T>(mEntityHandle);
        }

        template<typename T>
        bool HasComponent()
        {
            return mScene->mRegistry.all_of<T>(mEntityHandle);
        }

        template<typename T>
        void RemoveComponent()
        {
            HE_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!");
            mScene->mRegistry.remove<T>(mEntityHandle);
        }

        operator bool() const { return mEntityHandle != entt::null; }
        operator entt::entity() const { return mEntityHandle; }
        operator uint32_t() const { return (uint32_t)mEntityHandle; }

		UUID GetUUID() { return GetComponent<IDComponent>().ID; }
		const std::string& GetName() { return GetComponent<TagComponent>().Tag; }

        bool operator==(const Entity& other) const 
        { 
            return mEntityHandle == other.mEntityHandle && mScene == other.mScene; 
        }

        bool operator!=(const Entity& other) const
        {
            return !(*this == other);
        }
    private:
        entt::entity mEntityHandle{ entt::null };
        Scene* mScene = nullptr;
    };
}
