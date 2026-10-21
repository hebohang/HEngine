#pragma once

#include "Runtime/EcsFramework/System/System.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include <map>

namespace HEngine
{
    class PhysicSystem2D : public System
    {
	public:
		PhysicSystem2D(Level* level);
		~PhysicSystem2D() override;
    public:
		void OnRuntiemStart() override;
		void OnUpdateRuntime(Timestep ts) override;
		void OnRuntimeStop() override;
		bool SetBodyLinearVelocity(void* runtimeBody, const glm::vec2& velocity);
        bool GetBodyLinearVelocity(void* runtimeBody, glm::vec2& velocity);
        bool ApplyBodyForce(void* runtimeBody, const glm::vec2& force, bool impulse);
#if WITH_EDITOR
		void OnUpdateEditor(Timestep ts, EditorCamera& camera) override;
#endif
	private:
		void BuildRuntimeBodies();
		void OnBodyRemoving(Registry& registry, EntityHandle entity);
		entt::scoped_connection mBodyConnection;
		class b2World* mPhysicsWorld = nullptr;
		struct ContactState { glm::vec3 Point, Normal; bool Trigger; };
		std::map<std::pair<EntityHandle, EntityHandle>, ContactState> mContacts;
    };
}
