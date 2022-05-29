#include "hepch.h"

#include "Runtime/EcsFramework/System/Physics/PhysicSystem3D.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Component/Physics/3D/BoxCollider3DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/3D/ConvexHullComponent.h"
#include "Runtime/EcsFramework/Component/Physics/3D/Rigidbody3DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/3D/SphereCollider3DComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"

namespace HEngine
{
	Utils::BulletDrawer PhysicSystem3D::mDebugDrawer;

	void PhysicSystem3D::OnRuntiemStart()
	{
		mBroadphase = new btDbvtBroadphase();
		mCollisionConfiguration = new btDefaultCollisionConfiguration();
		mDispatcher = new btCollisionDispatcher(mCollisionConfiguration);
		mSolver = new btSequentialImpulseConstraintSolver();
		mDynamicsWorld = new btDiscreteDynamicsWorld(mDispatcher, mBroadphase, mSolver, mCollisionConfiguration);
		mDynamicsWorld->setGravity(btVector3(0.0f, -9.8f, 0.0f));
		//mDynamicsWorld->setForceUpdateAllAabbs(true);

		auto view = mLevel->mRegistry.view<TransformComponent, Rigidbody3DComponent>();
		for (auto e : view)
		{
			Entity entity = { e, mLevel };
			auto& transform = entity.GetComponent<TransformComponent>();
			auto& rb3d = entity.GetComponent<Rigidbody3DComponent>();

			if (entity.HasComponent<BoxCollider3DComponent>())
			{
				auto& bc3d = entity.GetComponent<BoxCollider3DComponent>();

				btCollisionShape* shape = new btBoxShape(btVector3(bc3d.Size.x * transform.Scale.x, bc3d.Size.y * transform.Scale.y, bc3d.Size.z * transform.Scale.z));
				btVector3 inertia{ 0.0f, 0.0f, 0.0f };
				if (rb3d.mass > 0.0f) shape->calculateLocalInertia(rb3d.mass, inertia);

				btTransform trans;
				trans.setOrigin(btVector3(transform.Translation.x, transform.Translation.y, transform.Translation.z));
				auto comQuat = glm::quat(transform.Rotation);
				btQuaternion btQuat;
				btQuat.setValue(comQuat.x, comQuat.y, comQuat.z, comQuat.w);
				trans.setRotation(btQuat);

				btDefaultMotionState* motion = new btDefaultMotionState(trans);
				btRigidBody::btRigidBodyConstructionInfo rbInfo(rb3d.mass, motion, shape, inertia);
				rbInfo.m_linearDamping = bc3d.linearDamping;
				rbInfo.m_angularDamping = bc3d.angularDamping;
				rbInfo.m_restitution = bc3d.restitution;
				rbInfo.m_friction = bc3d.friction;

				btRigidBody* body = new btRigidBody(rbInfo);
				body->setSleepingThresholds(0.01f, glm::radians(0.1f));
				body->setActivationState(DISABLE_DEACTIVATION);

				if (rb3d.Type == Rigidbody3DComponent::Body3DType::Static)
				{
					body->setCollisionFlags(body->getCollisionFlags() | btCollisionObject::CF_STATIC_OBJECT);
				}
				else if (rb3d.Type == Rigidbody3DComponent::Body3DType::Kinematic)
				{
					body->setCollisionFlags(body->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
				}
				else if (rb3d.Type == Rigidbody3DComponent::Body3DType::Dynamic)
				{
					body->setCollisionFlags(body->getCollisionFlags() | btCollisionObject::CF_DYNAMIC_OBJECT);
				}

				rb3d.RuntimeBody = body;
				mDynamicsWorld->addRigidBody(body);
			}
			else if (entity.HasComponent<SphereCollider3DComponent>())
			{
				auto& sc3d = entity.GetComponent<SphereCollider3DComponent>();

				btCollisionShape* shape = new btSphereShape(sc3d.radius * transform.Scale.x);
				btVector3 inertia{ 0.0f, 0.0f, 0.0f };
				if (rb3d.mass > 0.0f) shape->calculateLocalInertia(rb3d.mass, inertia);

				btTransform trans;
				trans.setOrigin(btVector3(transform.Translation.x, transform.Translation.y, transform.Translation.z));
				auto comQuat = glm::quat(transform.Rotation);
				btQuaternion btQuat;
				btQuat.setValue(comQuat.x, comQuat.y, comQuat.z, comQuat.w);
				trans.setRotation(btQuat);

				btDefaultMotionState* motion = new btDefaultMotionState(trans);
				btRigidBody::btRigidBodyConstructionInfo rbInfo(rb3d.mass, motion, shape, inertia);
				rbInfo.m_linearDamping = sc3d.linearDamping;
				rbInfo.m_angularDamping = sc3d.angularDamping;
				rbInfo.m_restitution = sc3d.restitution;
				rbInfo.m_friction = sc3d.friction;

				btRigidBody* body = new btRigidBody(rbInfo);
				body->setSleepingThresholds(0.01f, glm::radians(0.1f));
				body->setActivationState(DISABLE_DEACTIVATION);

				if (rb3d.Type == Rigidbody3DComponent::Body3DType::Static)
				{
					body->setCollisionFlags(body->getCollisionFlags() | btCollisionObject::CF_STATIC_OBJECT);
				}
				else if (rb3d.Type == Rigidbody3DComponent::Body3DType::Kinematic)
				{
					body->setCollisionFlags(body->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
				}
				else if (rb3d.Type == Rigidbody3DComponent::Body3DType::Dynamic)
				{
					body->setCollisionFlags(body->getCollisionFlags() | btCollisionObject::CF_DYNAMIC_OBJECT);
				}

				rb3d.RuntimeBody = body;
				mDynamicsWorld->addRigidBody(body);
			}
			else if (entity.HasComponent<ConvexHullComponent>() && entity.HasComponent<MeshComponent>())
			{
				auto& chc = entity.GetComponent<ConvexHullComponent>();
				auto& meshc = entity.GetComponent<MeshComponent>();

				btConvexHullShape* shape = new btConvexHullShape();

				for (const auto& submesh : meshc.mMesh->mSubMeshes)
				{
					auto& staticVertices = submesh.mStaticVertices;
					auto& skinnedVertices = submesh.mSkinnedVertices;
					for (const auto& vertex : staticVertices)
					{
						shape->addPoint(btVector3(vertex.Pos.x * transform.Scale.x, vertex.Pos.y * transform.Scale.y, vertex.Pos.z * transform.Scale.z));
					}
					for (const auto& vertex : skinnedVertices)
					{
						shape->addPoint(btVector3(vertex.Pos.x * transform.Scale.x, vertex.Pos.y * transform.Scale.y, vertex.Pos.z * transform.Scale.z));
					}
				}

				btVector3 inertia{ 0.0f, 0.0f, 0.0f };
				if (rb3d.mass > 0.0f) shape->calculateLocalInertia(rb3d.mass, inertia);

				btTransform trans;
				trans.setOrigin(btVector3(transform.Translation.x, transform.Translation.y, transform.Translation.z));
				auto comQuat = glm::quat(transform.Rotation);
				btQuaternion btQuat;
				btQuat.setValue(comQuat.x, comQuat.y, comQuat.z, comQuat.w);
				trans.setRotation(btQuat);

				btDefaultMotionState* motion = new btDefaultMotionState(trans);
				btRigidBody::btRigidBodyConstructionInfo rbInfo(rb3d.mass, motion, shape, inertia);
				rbInfo.m_linearDamping = chc.linearDamping;
				rbInfo.m_angularDamping = chc.angularDamping;
				rbInfo.m_restitution = chc.restitution;
				rbInfo.m_friction = chc.friction;

				btRigidBody* body = new btRigidBody(rbInfo);
				body->setSleepingThresholds(0.01f, glm::radians(0.1f));
				body->setActivationState(DISABLE_DEACTIVATION);

				if (rb3d.Type == Rigidbody3DComponent::Body3DType::Static)
				{
					body->setCollisionFlags(body->getCollisionFlags() | btCollisionObject::CF_STATIC_OBJECT);
				}
				else if (rb3d.Type == Rigidbody3DComponent::Body3DType::Kinematic)
				{
					body->setCollisionFlags(body->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
				}
				else if (rb3d.Type == Rigidbody3DComponent::Body3DType::Dynamic)
				{
					body->setCollisionFlags(body->getCollisionFlags() | btCollisionObject::CF_DYNAMIC_OBJECT);
				}

				rb3d.RuntimeBody = body;
				mDynamicsWorld->addRigidBody(body);
			}
		}
	}

	void PhysicSystem3D::OnUpdateRuntime(Timestep ts)
	{
		mDynamicsWorld->stepSimulation(ts, 10);

		auto view = mLevel->mRegistry.view<TransformComponent, Rigidbody3DComponent>();
		for (auto e : view)
		{
			Entity entity = { e, mLevel };
			auto componentsTuple = entity.GetComponents<TransformComponent, Rigidbody3DComponent>();
			auto [transform, rb3d] = componentsTuple;

			btRigidBody* body = (btRigidBody*)(*rb3d).RuntimeBody;
			const auto& trans = body->getWorldTransform();
			(*transform).Translation.x = trans.getOrigin().x();
			(*transform).Translation.y = trans.getOrigin().y();
			(*transform).Translation.z = trans.getOrigin().z();

			btScalar yawZ, pitchY, rollX;
			trans.getRotation().getEulerZYX(yawZ, pitchY, rollX);

			(*transform).Rotation.x = rollX;
			(*transform).Rotation.y = pitchY;
			(*transform).Rotation.z = yawZ;
		}

		if (ModeManager::bShowPhysicsColliders)
		{
			Entity camera = mLevel->GetPrimaryCameraEntity();
			Renderer2D::BeginScene(camera.GetComponent<CameraComponent>().Camera, camera.GetComponent<TransformComponent>().GetTransform());

			mDynamicsWorld->setDebugDrawer(&mDebugDrawer);
			mDynamicsWorld->debugDrawWorld();

			Renderer2D::EndScene();
		}
	}

	void PhysicSystem3D::OnRuntimeStop()
	{
		//auto view = mLevel->mRegistry.view<Rigidbody3DComponent>();
		//for (auto e : view)
		//{
		//	Entity entity = { e, mLevel };
		//	auto& rb3d = entity.GetComponent<Rigidbody3DComponent>();

		//	// it seems when you delete the btRigidBody,
		//	// the motion and the shape will also be deleted
		//	delete (btRigidBody*)rb3d.RuntimeBody; 
		//}

		// it seems we only need to delete the DynamicsWorld
		delete mDynamicsWorld;
		delete mSolver;
		delete mDispatcher;
		delete mCollisionConfiguration;
		delete mBroadphase;
	}

	void PhysicSystem3D::OnUpdateEditor(Timestep ts, EditorCamera& camera)
	{
		if (ModeManager::bShowPhysicsColliders)
		{
			// TEMP
			OnRuntiemStart();

			Renderer2D::BeginScene(camera);

			mDynamicsWorld->setDebugDrawer(&mDebugDrawer);
			mDynamicsWorld->debugDrawWorld();

			Renderer2D::EndScene();

			OnRuntimeStop();
		}
	}
}