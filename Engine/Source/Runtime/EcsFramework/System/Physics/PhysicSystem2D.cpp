#include "Hepch.h"

#include "Runtime/EcsFramework/System/Physics/PhysicSystem2D.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Physics/TwoD/BoxCollider2DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/TwoD/CircleCollider2DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/TwoD/Rigidbody2DComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/System/Script/ScriptSystem.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/Graphics/Renderer2D.h"

#include <box2d/b2_world.h>
#include <box2d/b2_body.h>
#include <box2d/b2_fixture.h>
#include <box2d/b2_polygon_shape.h>
#include <box2d/b2_circle_shape.h>
#include <box2d/b2_contact.h>

namespace HEngine
{
	PhysicSystem2D::PhysicSystem2D(Level* level) : System(level),
		mBodyConnection(level->mRegistry.on_destroy<Rigidbody2DComponent>().connect<&PhysicSystem2D::OnBodyRemoving>(*this)) {}
	PhysicSystem2D::~PhysicSystem2D() { OnRuntimeStop(); }
	void PhysicSystem2D::OnBodyRemoving(Registry& registry, EntityHandle entity)
	{
		auto& component = registry.get<Rigidbody2DComponent>(entity);
		if (mPhysicsWorld && component.RuntimeBody)
			mPhysicsWorld->DestroyBody(static_cast<b2Body*>(component.RuntimeBody));
		component.RuntimeBody = nullptr;
	}
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
		HE_PROFILE;

		if (mPhysicsWorld) OnRuntimeStop();
		mContacts.clear();
		mPhysicsWorld = new b2World({ 0.0f, -9.8f });
		BuildRuntimeBodies();
	}

	void PhysicSystem2D::BuildRuntimeBodies()
	{
		auto view = mLevel->mRegistry.view<TransformComponent, Rigidbody2DComponent>();
		for (auto e : view)
		{
			Entity entity = { e, mLevel };
			auto& rb2d = entity.GetComponent<Rigidbody2DComponent>();
			if (rb2d.RuntimeBody) continue;
			const glm::vec3 worldTranslation = mLevel->GetWorldTranslation(entity);
			const glm::vec3 worldRotation = mLevel->GetWorldRotation(entity);
			const glm::vec3 worldScale = mLevel->GetWorldScale(entity);

			b2BodyDef bodyDef;
			bodyDef.type = Rigidbody2DTypeToBox2DBody(rb2d.Type);
			bodyDef.position.Set(worldTranslation.x, worldTranslation.y);
			bodyDef.angle = worldRotation.z;
			bodyDef.userData.pointer = static_cast<uintptr_t>(e) + 1;

			b2Body* body = mPhysicsWorld->CreateBody(&bodyDef);
			body->SetFixedRotation(rb2d.FixedRotation);
			rb2d.RuntimeBody = body;

			if (entity.HasComponent<BoxCollider2DComponent>())
			{
				auto& bc2d = entity.GetComponent<BoxCollider2DComponent>();

				b2PolygonShape boxShape;
				boxShape.SetAsBox(bc2d.Size.x * worldScale.x, bc2d.Size.y * worldScale.y);

				b2FixtureDef fixtureDef;
				fixtureDef.shape = &boxShape;
				fixtureDef.density = bc2d.Density;
				fixtureDef.friction = bc2d.Friction;
				fixtureDef.restitution = bc2d.Restitution;
				fixtureDef.restitutionThreshold = bc2d.RestitutionThreshold;
				fixtureDef.isSensor = bc2d.IsTrigger;
				body->CreateFixture(&fixtureDef);
			}

			if (entity.HasComponent<CircleCollider2DComponent>())
			{
				//HE_CORE_ASSERT(entity.HasComponent<CircleRendererComponent>());
				auto& cc2d = entity.GetComponent<CircleCollider2DComponent>();

				b2CircleShape circleShape;
				circleShape.m_p.Set(cc2d.Offset.x, cc2d.Offset.y);
				// default CircleCollider2DComponent.Radius == default CircleRendererComponent.Radius
				circleShape.m_radius = cc2d.Radius * worldScale.x;

				b2FixtureDef fixtureDef;
				fixtureDef.shape = &circleShape;
				fixtureDef.density = cc2d.Density;
				fixtureDef.friction = cc2d.Friction;
				fixtureDef.restitution = cc2d.Restitution;
				fixtureDef.restitutionThreshold = cc2d.RestitutionThreshold;
				fixtureDef.isSensor = cc2d.IsTrigger;
				body->CreateFixture(&fixtureDef);
			}
		}
	}

	void PhysicSystem2D::OnUpdateRuntime(Timestep ts)
	{
		HE_PROFILE;
		if (!mPhysicsWorld) return;
		BuildRuntimeBodies();

		const int32_t velocityIterations = 6;
		const int32_t positionIterations = 2;
		mPhysicsWorld->Step(ts, velocityIterations, positionIterations);
		// Snapshot touching pairs after Box2D unlocks; multiple fixtures publish one
		// entity-pair event, and sleeping contacts remain present in the contact list.
		decltype(mContacts) current;
		for (auto* contact = mPhysicsWorld->GetContactList(); contact; contact = contact->GetNext())
		{
			if (!contact->IsTouching() || !contact->IsEnabled()) continue;
			auto* first = contact->GetFixtureA(); auto* second = contact->GetFixtureB();
			const auto firstId = first->GetBody()->GetUserData().pointer;
			const auto secondId = second->GetBody()->GetUserData().pointer;
			if (!firstId || !secondId) continue;
			const bool trigger = first->IsSensor() || second->IsSensor();
			b2WorldManifold manifold{};
			if (!trigger) contact->GetWorldManifold(&manifold);
			EntityHandle a = static_cast<EntityHandle>(firstId - 1), b = static_cast<EntityHandle>(secondId - 1);
			glm::vec3 normal = trigger ? glm::vec3(0) : glm::vec3(manifold.normal.x, manifold.normal.y, 0);
			const glm::vec3 point = trigger ? glm::vec3(0) : glm::vec3(manifold.points[0].x, manifold.points[0].y, 0);
			if (b < a) { std::swap(a, b); normal = -normal; }
			current.try_emplace(std::pair{a, b}, ContactState{point, normal, trigger});
			if (current.size() > 16384) throw std::runtime_error("2D script contact budget exceeded.");
		}
		if (auto* scripts = mLevel->TryGetSystem<ScriptSystem>())
		{
			const auto publish = [&](const auto& pair, const ContactState& contact, uint32_t phase) {
				const glm::vec3 point = phase == 2 ? glm::vec3(0) : contact.Point;
				const glm::vec3 normal = phase == 2 ? glm::vec3(0) : contact.Normal;
				scripts->QueueContact(pair.first, pair.second, phase, contact.Trigger, point, -normal);
				scripts->QueueContact(pair.second, pair.first, phase, contact.Trigger, point, normal);
			};
			for (const auto& [pair, contact] : current) publish(pair, contact, mContacts.contains(pair) ? 1u : 0u);
			for (const auto& [pair, contact] : mContacts) if (!current.contains(pair)) publish(pair, contact, 2);
		}
		mContacts = std::move(current);

		// Retrieve transform from Box2D
		auto view = mLevel->mRegistry.view<TransformComponent, Rigidbody2DComponent>();
		for (auto e : view)
		{
			Entity entity = { e, mLevel };
			auto& rb2d = entity.GetComponent<Rigidbody2DComponent>();

			b2Body* body = (b2Body*)rb2d.RuntimeBody;
			const auto& position = body->GetPosition();

			glm::vec3 worldTranslation(0.0f);
			glm::vec3 worldRotation(0.0f);
			glm::vec3 worldScale(1.0f);
			mLevel->GetWorldTransformComponents(entity, worldTranslation, worldRotation, worldScale);
			worldTranslation.x = position.x;
			worldTranslation.y = position.y;
			worldRotation.z = body->GetAngle();
			mLevel->SetWorldTransformComponents(entity, worldTranslation, worldRotation, worldScale);
		}

		if (ModeManager::bShowPhysicsColliders)
		{
			Entity camera = mLevel->GetPrimaryCameraEntity();
			if (!camera)
				return;
			Renderer2D::BeginScene(camera.GetComponent<CameraComponent>().Camera, mLevel->GetWorldTransform(camera));

			{
				auto view = mLevel->GetAllEntitiesWith<TransformComponent, BoxCollider2DComponent>();
				for (auto entity : view)
				{
					auto [tc, bc2d] = view.get<TransformComponent, BoxCollider2DComponent>(entity);
					Entity boxEntity = { entity, mLevel };
					const glm::vec3 worldTranslation = mLevel->GetWorldTranslation(boxEntity);
					const glm::vec3 worldRotation = mLevel->GetWorldRotation(boxEntity);
					const glm::vec3 worldScale = mLevel->GetWorldScale(boxEntity);

					glm::vec3 translation = worldTranslation + glm::vec3(bc2d.Offset, 0.001f);
					glm::vec3 scale = worldScale * glm::vec3(bc2d.Size * 2.0f, 1.0f);

					glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation)
						* glm::rotate(glm::mat4(1.0f), worldRotation.z, glm::vec3(0.0f, 0.0f, 1.0f))
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
					Entity circleEntity = { entity, mLevel };
					const glm::vec3 worldTranslation = mLevel->GetWorldTranslation(circleEntity);
					const glm::vec3 worldScale = mLevel->GetWorldScale(circleEntity);

					glm::vec3 translation = worldTranslation + glm::vec3(cc2d.Offset, 0.001f);
					glm::vec3 scale = worldScale * glm::vec3(cc2d.Radius * 2.0f);

					glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation)
						* glm::scale(glm::mat4(1.0f), scale);

					//glm::mat4 transform = glm::translate(tc.GetTransform(), glm::vec3(0, 0, 0.01f));

					Renderer2D::DrawCircle(transform, glm::vec4(0, 1, 0, 1), 0.05f);
				}
			}

			Renderer2D::EndScene();
		}
	}

	bool PhysicSystem2D::SetBodyLinearVelocity(void* runtimeBody, const glm::vec2& velocity)
	{
		if (!mPhysicsWorld || !runtimeBody || !std::isfinite(velocity.x) || !std::isfinite(velocity.y)) return false;
		for (auto* body = mPhysicsWorld->GetBodyList(); body; body = body->GetNext())
			if (body == runtimeBody) { body->SetLinearVelocity({velocity.x, velocity.y}); return true; }
		return false;
	}

	void PhysicSystem2D::OnRuntimeStop()
	{
		HE_PROFILE;

		delete mPhysicsWorld;
		mPhysicsWorld = nullptr;
		for (auto entity : mLevel->mRegistry.view<Rigidbody2DComponent>())
			mLevel->mRegistry.get<Rigidbody2DComponent>(entity).RuntimeBody = nullptr;
		mContacts.clear();
	}

#if WITH_EDITOR
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
					Entity boxEntity = { entity, mLevel };
					const glm::vec3 worldTranslation = mLevel->GetWorldTranslation(boxEntity);
					const glm::vec3 worldRotation = mLevel->GetWorldRotation(boxEntity);
					const glm::vec3 worldScale = mLevel->GetWorldScale(boxEntity);

					glm::vec3 translation = worldTranslation + glm::vec3(bc2d.Offset, 0.001f);
					glm::vec3 scale = worldScale * glm::vec3(bc2d.Size * 2.0f, 1.0f);

					glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation)
						* glm::rotate(glm::mat4(1.0f), worldRotation.z, glm::vec3(0.0f, 0.0f, 1.0f))
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
					Entity circleEntity = { entity, mLevel };
					const glm::vec3 worldTranslation = mLevel->GetWorldTranslation(circleEntity);
					const glm::vec3 worldScale = mLevel->GetWorldScale(circleEntity);

					glm::vec3 translation = worldTranslation + glm::vec3(cc2d.Offset, 0.001f);
					glm::vec3 scale = worldScale * glm::vec3(cc2d.Radius * 2.0f);

					glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation)
						* glm::scale(glm::mat4(1.0f), scale);

					//glm::mat4 transform = glm::translate(tc.GetTransform(), glm::vec3(0, 0, 0.01f));

									Renderer2D::DrawCircle(transform, glm::vec4(0, 1, 0, 1), 0.05f);
								}
							}

								Renderer2D::EndScene();
							}
						}
					#endif
					}
