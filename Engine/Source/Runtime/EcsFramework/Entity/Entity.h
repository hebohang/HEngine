#pragma once

#include "Runtime/Core/UUID.h"
#include "Runtime/EcsFramework/Level/Level.h"

#include <entt.hpp>

#include <tuple>
#include <type_traits>

namespace HEngine
{
    class Entity
    {
    public:
        Entity() = default;
        Entity(EntityHandle handle, Level* level);
        Entity(const Entity& other) = default;

        template<typename T, typename... Args>
        T& AddComponent(Args&&... args)
        {
            HE_CORE_ASSERT(!HasComponent<T>(), "Entity already has component!");
            T& comp = mLevel->mRegistry.emplace<T>(mEntityHandle, std::forward<Args>(args)...);
            mLevel->OnComponentAdded<T>(*this, comp);
            return comp;
        }

        template<typename T>
        T& AddComponent(const T& component)
        {
            HE_CORE_ASSERT(!HasComponent<T>(), "Entity already has component!");
            T& comp = mLevel->mRegistry.emplace<T>(mEntityHandle, component);
            mLevel->OnComponentAdded<T>(*this, comp);
            return comp;
        }

		template<typename T, typename... Args>
		T& AddOrReplaceComponent(Args&&... args)
		{
			if constexpr (std::is_same_v<T, MeshComponent>)
			{
				if (HasComponent<T>())
					mLevel->OnMeshComponentRemoving(*this);
			}
			T& comp = mLevel->mRegistry.emplace_or_replace<T>(mEntityHandle, std::forward<Args>(args)...);
			mLevel->OnComponentAdded<T>(*this, comp);
			return comp;
		}

        template<typename T>
        T& AddOrReplaceComponent(const T& component)
        {
			if constexpr (std::is_same_v<T, MeshComponent>)
			{
				if (HasComponent<T>())
					mLevel->OnMeshComponentRemoving(*this);
			}
            T& comp = mLevel->mRegistry.emplace_or_replace<T>(mEntityHandle, component);
            mLevel->OnComponentAdded<T>(*this, comp);
            return comp;
        }

        template<typename T>
        T& GetComponent()
        {
            HE_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!");
            return mLevel->mRegistry.get<T>(mEntityHandle);
        }

        template<typename... T>
        std::tuple<T*...> GetComponents()
        {
            HE_CORE_ASSERT((HasComponent<T>() && ...), "Entity does not have component!");
            return std::make_tuple<T*...>((&mLevel->mRegistry.get<T>(mEntityHandle))...);
        }

        template<typename... T>
        std::tuple<const T* const...> GetConstComponents()
        {
            HE_CORE_ASSERT((HasComponent<T>() && ...), "Entity does not have component!");
            return std::make_tuple(((const T* const)&mLevel->mRegistry.get<T>(mEntityHandle))...);
        }

        template<typename T>
        bool HasComponent()
        {
            if (!IsValid())
                return false;

            return mLevel->mRegistry.all_of<T>(mEntityHandle);
        }

        [[nodiscard]] bool IsValid() const
        {
            return mLevel && mEntityHandle != entt::null && mLevel->mRegistry.valid(mEntityHandle);
        }

        template<typename T>
		void RemoveComponent()
        {
            HE_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!");
			if constexpr (std::is_same_v<T, MeshComponent>)
			{
				mLevel->OnMeshComponentRemoving(*this);
			}
			else if constexpr (std::is_same_v<T, PythonScriptComponent>)
			{
				mLevel->OnPythonScriptComponentRemoving(*this);
				// A Python Stop callback may destroy this entity recursively. In that
				// case the nested removal owns the registry mutation.
				if (!IsValid() || !mLevel->mRegistry.all_of<T>(mEntityHandle))
					return;
			}
            mLevel->mRegistry.remove<T>(mEntityHandle);
        }

        operator bool() const { return IsValid(); }
        operator EntityHandle() const { return mEntityHandle; }
        operator uint32_t() const { return (uint32_t)mEntityHandle; }

        [[nodiscard]] UUID UUID();
        [[nodiscard]] const std::string& GetName();
        [[nodiscard]] Level* GetLevel() const { return mLevel; }

        bool operator==(const Entity& other) const 
        { 
            return mEntityHandle == other.mEntityHandle && mLevel == other.mLevel; 
        }

        bool operator!=(const Entity& other) const
        {
            return !(*this == other);
        }
    private:
        EntityHandle mEntityHandle{ entt::null };
        Level* mLevel = nullptr;
    };
}
