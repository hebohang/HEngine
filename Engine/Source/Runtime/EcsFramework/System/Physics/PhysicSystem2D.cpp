#include "hepch.h"

#include "Runtime/EcsFramework/System/Physics/PhysicSystem2D.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Physics/2D/BoxCollider2DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/2D/CircleCollider2DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/2D/Rigidbody2DComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/Renderer/Renderer2D.h"

#include <box2d/b2_world.h>
#include <box2d/b2_body.h>
#include <box2d/b2_fixture.h>
#include <box2d/b2_polygon_shape.h>
#include <box2d/b2_circle_shape.h>

namespace HEngine
{
	static b2BodyType Rigidbody2DTypeToBox2DBody(Rigidbody2DComponent::BodyType bodyType)
	{
		switch (bodyType)
		{
		case HEngine::Rigidbody2DComponent::BodyType::Static:		return b2_staticBody;
		case HEngine::Rigidbody2DComponent::BodyType::Dynamic:		return b2_dynamicBody;
		case HEngine::Rigidbody2DComponent::BodyType::Kinematic:	return b2_kinematicBody;
		default:													break;
		}

		HE_CORE_ASSERT(false, "Unknown body type");
		return b2_staticBody;
	}

	void PhysicSystem2D::OnRuntiemStart()
	{
		mPhysicsWorld = new b2World({ 0.0f, -9.8f });

		auto view = mLevel->mRegistry.view<Rigidbody2DComponent>();
		for (auto e : view)
		{
			Entity entity = { e, mLevel };
			auto& transform = entity.GetComponent<TransformComponent>();
			auto& rb2d = entity.GetComponent<Rigidbody2DComponent>();

			b2BodyDef bodyDef;
			bodyDef.type = Rigidbody2DTypeToBox2DBody(rb2d.Type);
			bodyDef.position.Set(transform.Translation.x, transform.Translation.y);
			bodyDef.angle = transform.Rotation.z;

			b2Body* body = mPhysicsWorld->CreateBody(&bodyDef);
			body->SetFixedRotation(rb2d.FixedRotation);
			rb2d.RuntimeBody = body;

			if (entity.HasComponent<BoxCollider2DComponent>())
			{
				auto& bc2d = entity.GetComponent<BoxCollider2DComponent>();

				b2PolygonShape boxShape;
				boxShape.SetAsBox(bc2d.Size.x * transform.Scale.x, bc2d.Size.y * transform.Scale.y);

				b2FixtureDef fixtureDef;
				fixtureDef.shape = &boxShape;
				fixtureDef.density = bc2d.Density;
				fixtureDef.friction = bc2d.Friction;
				fixtureDef.restitution = bc2d.Restitution;
				fixtureDef.restitutionThreshold = bc2d.RestitutionThreshold;
				body->CreateFixture(&fixtureDef);
			}

			if (entity.HasComponent<CircleCollider2DComponent>())
			{
				//HE_CORE_ASSERT(entity.HasComponent<CircleRendererComponent>());
				auto& cc2d = entity.GetComponent<CircleCollider2DComponent>();

				b2CircleShape circleShape;
				circleShape.m_p.Set(cc2d.Offset.x, cc2d.Offset.y);
				// default CircleCollider2DComponent.Radius == default CircleRendererComponent.Radius
				circleShape.m_radius = cc2d.Radius * transform.Scale.x;

				b2FixtureDef fixtureDef;
				fixtureDef.shape = &circleShape;
				fixtureDef.density = cc2d.Density;
				fixtureDef.friction = cc2d.Friction;
				fixtureDef.restitution = cc2d.Restitution;
				fixtureDef.restitutionThreshold = cc2d.RestitutionThreshold;
				body->CreateFixture(&fixtureDef);
			}
		}
	}

	void PhysicSystem2D::OnUpdateRuntime(Timestep ts)
	{
		const int32_t velocityIterations = 6;
		const int32_t positionIterations = 2;
		mPhysicsWorld->Step(ts, velocityIterations, positionIterations);

		// Retrieve transform from Box2D
		auto view = mLevel->mRegistry.view<TransformComponent, Rigidbody2DComponent>();
		for (auto e : view)
		{
			Entity entity = { e, mLevel };
			auto componentsTuple = entity.GetComponents<TransformComponent, Rigidbody2DComponent>();
			auto [transform, rb2d] = componentsTuple;

			b2Body* body = (b2Body*)(*rb2d).RuntimeBody;
			const auto& position = body->GetPosition();
			(*transform).Translation.x = position.x;
			(*transform).Translation.y = position.y;
			(*transform).Rotation.z = body->GetAngle();
		}

		if (ModeManager::bShowPhysicsColliders)
		{
			Entity camera = mLevel->GetPrimaryCameraEntity();
			Renderer2D::BeginScene(camera.GetComponent<CameraComponent>().Camera, camera.GetComponent<TransformComponent>().GetTransform());

			{
				auto view = mLevel->GetAllEntitiesWith<TransformComponent, BoxCollider2DComponent>();
				for (auto entity : view)
				{
					auto [tc, bc2d] = view.get<TransformComponent, BoxCollider2DComponent>(entity);

					glm::vec3 translation = tc.Translation + glm::vec3(bc2d.Offset, 0.001f);
					glm::vec3 scale = tc.Scale * glm::vec3(bc2d.Size * 2.0f, 1.0f);

					glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation)
						* glm::rotate(glm::mat4(1.0f), tc.Rotation.z, glm::vec3(0.0f, 0.0f, 1.0f))
						* glm::scale(glm::mat4(1.0f), scale);

					//glm::mat4 transform = glm::translate(tc.GetTransform(), glm::vec3(0, 0, 0.01f));

					Renderer2D::DrawRect(transform, glm::vec4(0, 1, 0, 1));
				}
			}

			{
				auto view = mLevel->GetAllEntitiesWith<TransformComponent, CircleCollider2DComponent>();
				for (auto entity : view)
				{
					auto [tc, cc2d] = view.get<TransformComponent, CircleCollider2DComponent>(entity);

					glm::vec3 translation = tc.Translation + glm::vec3(cc2d.Offset, 0.001f);
					glm::vec3 scale = tc.Scale * glm::vec3(cc2d.Radius * 2.0f);

					glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation)
						* glm::scale(glm::mat4(1.0f), scale);

					//glm::mat4 transform = glm::translate(tc.GetTransform(), glm::vec3(0, 0, 0.01f));

					Renderer2D::DrawCircle(transform, glm::vec4(0, 1, 0, 1), 0.05f);
				}
			}

			Renderer2D::EndScene();
		}
	}

	void PhysicSystem2D::OnRuntimeStop()
	{
		delete mPhysicsWorld;
		mPhysicsWorld = nullptr;
	}

	void PhysicSystem2D::OnUpdateEditor(Timestep ts, EditorCamera& camera)
	{
		if (ModeManager::bShowPhysicsColliders)
		{
			Renderer2D::BeginScene(camera);

			{
				auto view = mLevel->GetAllEntitiesWith<TransformComponent, BoxCollider2DComponent>();
				for (auto entity : view)
				{
					auto [tc, bc2d] = view.get<TransformComponent, BoxCollider2DComponent>(entity);

					glm::vec3 translation = tc.Translation + glm::vec3(bc2d.Offset, 0.001f);
					glm::vec3 scale = tc.Scale * glm::vec3(bc2d.Size * 2.0f, 1.0f);

					glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation)
						* glm::rotate(glm::mat4(1.0f), tc.Rotation.z, glm::vec3(0.0f, 0.0f, 1.0f))
						* glm::scale(glm::mat4(1.0f), scale);

					//glm::mat4 transform = glm::translate(tc.GetTransform(), glm::vec3(0, 0, 0.01f));

					Renderer2D::DrawRect(transform, glm::vec4(0, 1, 0, 1));
				}
			}

			{
				auto view = mLevel->GetAllEntitiesWith<TransformComponent, CircleCollider2DComponent>();
				for (auto entity : view)
				{
					auto [tc, cc2d] = view.get<TransformComponent, CircleCollider2DComponent>(entity);

					glm::vec3 translation = tc.Translation + glm::vec3(cc2d.Offset, 0.001f);
					glm::vec3 scale = tc.Scale * glm::vec3(cc2d.Radius * 2.0f);

					glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation)
						* glm::scale(glm::mat4(1.0f), scale);

					//glm::mat4 transform = glm::translate(tc.GetTransform(), glm::vec3(0, 0, 0.01f));

					Renderer2D::DrawCircle(transform, glm::vec4(0, 1, 0, 1), 0.05f);
				}
			}

			Renderer2D::EndScene();
		}
	}
}