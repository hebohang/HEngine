#pragma once

#include "Runtime/Core/Timestep.h"
#include "Runtime/Core/UUID.h"
#include "Runtime/Core/Base/Base.h"
#include "Runtime/Camera/EditorCamera.h"

#include <entt.hpp>
#include <unordered_map>

class b2World;

namespace HEngine
{
    class Entity;

    class Level
    {
    public:
        Level();
        ~Level();

		static Ref<Level> Copy(Ref<Level> other);

        void ChangeDimMode();

        Entity CreateEntity(const std::string& name = std::string());
        Entity CreateEntityWithUUID(UUID uuid, const std::string& name = std::string());
        void DestroyEntity(Entity entity);

		void OnRuntimeStart();
		void OnRuntimeStop();

        void OnUpdateRuntime(Timestep ts);
        void OnUpdateEditor(Timestep ts, EditorCamera& camera);
        void OnViewportResize(uint32_t width, uint32_t height);

		void DuplicateEntity(Entity entity);

		Entity GetPrimaryCameraEntity();

		template<typename... Components>
		auto GetAllEntitiesWith()
		{
			return mRegistry.view<Components...>();
		}
    private:
        template<typename T>
        void OnComponentAdded(Entity entity, T& component);
    public:
        entt::registry mRegistry;
    private:
        uint32_t mViewportWidth = 0, mViewportHeight = 0;

		b2World* mPhysicsWorld = nullptr;

        std::vector<Scope<class System>> mSystems;
        //std::unordered_map<UUID, >

        friend class Entity;
        friend class SceneSerializer;
        friend class SceneHierarchyPanel;
    };
}
