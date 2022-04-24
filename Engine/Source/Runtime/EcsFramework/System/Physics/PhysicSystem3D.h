#pragma once

#include "Runtime/EcsFramework/System/System.h"
#include "Runtime/EcsFramework/Level/Level.h"

#include <btBulletCollisionCommon.h>
#include <btBulletDynamicsCommon.h>

namespace HEngine
{
    class PhysicSystem3D : public System
    {
	public:
		PhysicSystem3D(Level* level) : System(level) {}
		virtual ~PhysicSystem3D() = default;
    public:
		void OnRuntiemStart() override;
		void OnUpdateRuntime(Timestep ts) override;
		void OnRuntimeStop() override;
	private:
		btBroadphaseInterface* mBroadphase;
		btDefaultCollisionConfiguration* mCollisionConfiguration;
		btCollisionDispatcher* mDispatcher;
		btSequentialImpulseConstraintSolver* mSolver;
		btDiscreteDynamicsWorld* mDynamicsWorld;
    };
}