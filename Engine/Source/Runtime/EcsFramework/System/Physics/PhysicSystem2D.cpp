#include "hepch.h"

#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/EcsFramework/System/Physics/PhysicSystem2D.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/Entity/Entity.h"

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

	void PhysicSystem2D::OnUpdate(Timestep ts)
	{
		if (!ModeManager::IsEditState())
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
		}
	}

	void PhysicSystem2D::OnRuntimeStop()
	{
		delete mPhysicsWorld;
		mPhysicsWorld = nullptr;
	}
}