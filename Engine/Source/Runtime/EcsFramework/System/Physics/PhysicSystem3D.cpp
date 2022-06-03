#include "hepch.h"

#include "Runtime/EcsFramework/System/Physics/PhysicSystem3D.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Component/Physics/3D/Rigidbody3DComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/Utils/MathUtils/MathUtils.h"

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

			btTransform trans;
			btCollisionShape* shape;
			btVector3 inertia{ 0.0f, 0.0f, 0.0f };

			if (rb3d.Shape == CollisionShape::Box)
			{
				// Calculate Obb
				HE_CORE_ASSERT(entity.HasComponent<MeshComponent>());
				auto& transform = entity.GetComponent<TransformComponent>();
				auto& mesh = entity.GetComponent<MeshComponent>();

				std::vector<glm::vec3> vertices;
				for (const auto& subMesh : mesh.mMesh->mSubMeshes)
				{
					if (subMesh.mStaticVertices.empty())
					{
						for (const auto& vertex : subMesh.mSkinnedVertices)
						{
							vertices.emplace_back(vertex.Pos);
						}
					}
					else
					{
						for (const auto& vertex : subMesh.mStaticVertices)
						{
							vertices.emplace_back(vertex.Pos);
						}
					}
				}

				glm::vec3 originPos(0.0f);
				glm::mat3 covMat = Math::CalculateCovMatrix(vertices, originPos);

				glm::vec3 eValues;
				glm::mat3 eVectors;
				Math::JacobiSolver(covMat, eValues, eVectors);

				for (int i = 0; i < 3; i++)
				{
					if (eValues[i] == 0 || i == 2)
					{
						Math::SchmidtOrthogonalization(eVectors[(i + 1) % 3], eVectors[(i + 2) % 3], eVectors[i]);
						break;
					}
				}

				constexpr float infinity = std::numeric_limits<float>::infinity();
				glm::vec3 minExtents(infinity, infinity, infinity);
				glm::vec3 maxExtents(-infinity, -infinity, -infinity);

				for (const glm::vec3& displacement : vertices)
				{
					minExtents[0] = std::min(minExtents[0], glm::dot(displacement, eVectors[0]));
					minExtents[1] = std::min(minExtents[1], glm::dot(displacement, eVectors[1]));
					minExtents[2] = std::min(minExtents[2], glm::dot(displacement, eVectors[2]));

					maxExtents[0] = std::max(maxExtents[0], glm::dot(displacement, eVectors[0]));
					maxExtents[1] = std::max(maxExtents[1], glm::dot(displacement, eVectors[1]));
					maxExtents[2] = std::max(maxExtents[2], glm::dot(displacement, eVectors[2]));
				}

				glm::vec3 halfExtent = (maxExtents - minExtents) / 2.0f;
				glm::vec3 offset = halfExtent + minExtents;
				originPos += offset.x * eVectors[0] + offset.y * eVectors[1] + offset.z * eVectors[2];
				glm::vec3 offsetScale = originPos * (transform.Scale - 1.0f);
				originPos += offsetScale;
				originPos = glm::mat3(transform.GetRotationMatrix()) * originPos;
				originPos += transform.Translation;

				shape = new btBoxShape(btVector3(halfExtent.x * transform.Scale.x, halfExtent.y * transform.Scale.y, halfExtent.z * transform.Scale.z));
				if (rb3d.mass > 0.0f) shape->calculateLocalInertia(rb3d.mass, inertia);

				trans.setOrigin(Utils::GlmToBtVec3(originPos));

				auto comQuat = glm::quat(transform.Rotation) * glm::quat(glm::mat4(eVectors));
				btQuaternion btQuat;
				btQuat.setValue(comQuat.x, comQuat.y, comQuat.z, comQuat.w);
				trans.setRotation(btQuat);
			}
			else if (rb3d.Shape == CollisionShape::Sphere)
			{
				shape = new btSphereShape(transform.Scale.x);
				if (rb3d.mass > 0.0f) shape->calculateLocalInertia(rb3d.mass, inertia);

				trans.setOrigin(btVector3(transform.Translation.x, transform.Translation.y, transform.Translation.z));
			
				auto comQuat = glm::quat(transform.Rotation);
				btQuaternion btQuat;
				btQuat.setValue(comQuat.x, comQuat.y, comQuat.z, comQuat.w);
				trans.setRotation(btQuat);
			}
			else if (rb3d.Shape == CollisionShape::ConvexHull && entity.HasComponent<MeshComponent>())
			{
				auto& meshc = entity.GetComponent<MeshComponent>();

				shape = new btConvexHullShape();

				for (const auto& submesh : meshc.mMesh->mSubMeshes)
				{
					auto& staticVertices = submesh.mStaticVertices;
					auto& skinnedVertices = submesh.mSkinnedVertices;
					for (const auto& vertex : staticVertices)
					{
						static_cast<btConvexHullShape*>(shape)->addPoint(btVector3(vertex.Pos.x * transform.Scale.x, vertex.Pos.y * transform.Scale.y, vertex.Pos.z * transform.Scale.z));
					}
					for (const auto& vertex : skinnedVertices)
					{
						static_cast<btConvexHullShape*>(shape)->addPoint(btVector3(vertex.Pos.x * transform.Scale.x, vertex.Pos.y * transform.Scale.y, vertex.Pos.z * transform.Scale.z));
					}
				}

				if (rb3d.mass > 0.0f) shape->calculateLocalInertia(rb3d.mass, inertia);

				trans.setOrigin(btVector3(transform.Translation.x, transform.Translation.y, transform.Translation.z));
			
				auto comQuat = glm::quat(transform.Rotation);
				btQuaternion btQuat;
				btQuat.setValue(comQuat.x, comQuat.y, comQuat.z, comQuat.w);
				trans.setRotation(btQuat);
			}

			btDefaultMotionState* motion = new btDefaultMotionState(trans);
			btRigidBody::btRigidBodyConstructionInfo rbInfo(rb3d.mass, motion, shape, inertia);
			rbInfo.m_linearDamping = rb3d.linearDamping;
			rbInfo.m_angularDamping = rb3d.angularDamping;
			rbInfo.m_restitution = rb3d.restitution;
			rbInfo.m_friction = rb3d.friction;

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
		static bool initFlag = true;
		if (ModeManager::bShowPhysicsColliders)
		{
			//if (initFlag)
				OnRuntiemStart();

			Renderer2D::BeginScene(camera);

			mDynamicsWorld->setDebugDrawer(&mDebugDrawer);
			mDynamicsWorld->debugDrawWorld();

			Renderer2D::EndScene();

			OnRuntimeStop();
			//initFlag = false;
		}
		else
		{
			//if (!initFlag)
				//OnRuntimeStop();
			//initFlag = true;
		}
	}
}