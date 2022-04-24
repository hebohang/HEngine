#include "hepch.h"

#include "Runtime/Resource/ModeManager/ModeManager.h"

#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/System/SystemGroup.h"

#include "Runtime/Renderer/Renderer2D.h"
#include "Runtime/Renderer/Renderer3D.h"

#include <glm/glm.hpp>
#include <box2d/b2_world.h>
#include <box2d/b2_body.h>
#include <box2d/b2_fixture.h>
#include <box2d/b2_polygon_shape.h>
#include <box2d/b2_circle_shape.h>

namespace HEngine
{
    Level::Level()
    {
		if (ModeManager::b3DMode)
		{
			mSystems.emplace_back(CreateScope<RenderSystem3D>(this));
			mSystems.emplace_back(CreateScope<PhysicSystem3D>(this));
		}
		else
		{
			mSystems.emplace_back(CreateScope<PhysicSystem2D>(this));
			mSystems.emplace_back(CreateScope<NativeScriptSystem>(this));
			mSystems.emplace_back(CreateScope<RenderSystem2D>(this));
		}
    }

    Level::~Level()
    {
    }

	template<Component... C>
	static void CopyComponent(entt::registry& dst, entt::registry& src, const std::unordered_map<UUID, entt::entity>& enttMap)
	{
		([&]()
			{
				auto view = src.view<C>();
				for (auto srcEntity : view)
				{
					entt::entity dstEntity = enttMap.at(src.get<IDComponent>(srcEntity).ID);

					auto& srcComponent = src.get<C>(srcEntity);
					dst.emplace_or_replace<C>(dstEntity, srcComponent);
				}
			}(), ...);
	}

	template<Component... C>
	static void CopyComponent(ComponentGroup<C...>, entt::registry& dst, entt::registry& src, const std::unordered_map<UUID, entt::entity>& enttMap)
	{
		CopyComponent<C...>(dst, src, enttMap);
	}

	template<Component... C>
	static void CopyComponentIfExists(Entity dst, Entity src)
	{
		([&]()
			{
				if (src.HasComponent<C>())
					dst.AddOrReplaceComponent<C>(src.GetComponent<C>());
			}(), ...);
	}

	template<Component... C>
	static void CopyComponentIfExists(ComponentGroup<C...>, Entity dst, Entity src)
	{
		CopyComponentIfExists<C...>(dst, src);
	}

	Ref<Level> Level::Copy(Ref<Level> scene)
	{
		Ref<Level> newScene = CreateRef<Level>();

		newScene->mViewportWidth = scene->mViewportWidth;
		newScene->mViewportHeight = scene->mViewportHeight;

		std::unordered_map<UUID, entt::entity> enttMap;

		// Create entities in new scene
		auto& srcSceneRegistry = scene->mRegistry;
		auto& dstSceneRegistry = newScene->mRegistry;
		auto idView = srcSceneRegistry.view<IDComponent>();
		for (auto e : idView)
		{
			UUID uuid = srcSceneRegistry.get<IDComponent>(e).ID;
			const auto& name = srcSceneRegistry.get<TagComponent>(e).Tag;
			Entity newEntity = newScene->CreateEntityWithUUID(uuid, name);
			enttMap[uuid] = newEntity;
		}

		// Copy components (except IDComponent and TagComponent)
		CopyComponent(AllComponents{}, dstSceneRegistry, srcSceneRegistry, enttMap);

		return newScene;
	}

	void Level::ChangeDimMode()
	{
		int nowDimMode = ModeManager::b3DMode;
		if (nowDimMode)
		{
			mSystems.clear();
			mSystems.emplace_back(CreateScope<RenderSystem3D>(this));
			mSystems.emplace_back(CreateScope<PhysicSystem3D>(this));
		}
		else
		{
			mSystems.clear();
			mSystems.emplace_back(CreateScope<PhysicSystem2D>(this));
			mSystems.emplace_back(CreateScope<NativeScriptSystem>(this));
			mSystems.emplace_back(CreateScope<RenderSystem2D>(this));
		}
	}

    Entity Level::CreateEntity(const std::string& name)
    {
		return CreateEntityWithUUID(UUID(), name);
    }

	Entity Level::CreateEntityWithUUID(UUID uuid, const std::string& name)
	{
		Entity entity = { mRegistry.create(), this };
		entity.AddComponent<IDComponent>(uuid);
		entity.AddComponent<TransformComponent>();
		auto& tag = entity.AddComponent<TagComponent>();
		tag.Tag = name.empty() ? "Entity" : name;
		return entity;
	}

    void Level::DestroyEntity(Entity entity)
    {
        mRegistry.destroy(entity);
    }

	void Level::OnRuntimeStart()
	{
		for (auto& system : mSystems)
		{
			system->OnRuntiemStart();
		}
	}

	void Level::OnRuntimeStop()
	{
		for (auto& system : mSystems)
		{
			system->OnRuntimeStop();
		}
	}

	void Level::OnUpdateRuntime(Timestep ts)
	{
		for (auto& system : mSystems)
		{
			system->OnUpdateRuntime(ts);
		}
	}

	void Level::OnUpdateEditor(Timestep ts, EditorCamera& camera)
	{
		for (auto& system : mSystems)
		{
			system->OnUpdateEditor(ts, camera);
		}
	}

    void Level::OnViewportResize(uint32_t width, uint32_t height)
    {
        mViewportWidth = width;
        mViewportHeight = height;

        // Resize our non-FixedAspectRatio cameras
        auto view = mRegistry.view<CameraComponent>();
        for (auto entity : view)
        {
            auto& cameraComponent = view.get<CameraComponent>(entity);
            if (!cameraComponent.FixedAspectRatio)
                cameraComponent.Camera.SetViewportSize(width, height);
        }
    }

	void Level::DuplicateEntity(Entity entity)
	{
		Entity newEntity = CreateEntity(entity.GetName());

		CopyComponentIfExists(AllComponents{}, newEntity, entity);
	}

	Entity Level::GetPrimaryCameraEntity()
	{
		auto view = mRegistry.view<CameraComponent>();
		for (auto entity : view)
		{
			const auto& camera = view.get<CameraComponent>(entity);
			if (camera.Primary)
				return Entity{ entity, this };
		}
		return {};
	}

    template<typename T>
    void Level::OnComponentAdded(Entity entity, T& component)
    {
    }

    template<>
    void Level::OnComponentAdded<IDComponent>(Entity entity, IDComponent& component)
    {
    }

	template<>
	void Level::OnComponentAdded<TransformComponent>(Entity entity, TransformComponent& component)
	{
	}

    template<>
    void Level::OnComponentAdded<CameraComponent>(Entity entity, CameraComponent& component)
    {
		if (mViewportWidth > 0 && mViewportHeight > 0)
			component.Camera.SetViewportSize(mViewportWidth, mViewportHeight);
    }

    template<>
    void Level::OnComponentAdded<SpriteRendererComponent>(Entity entity, SpriteRendererComponent& component)
    {
    }

	template<>
	void Level::OnComponentAdded<CircleRendererComponent>(Entity entity, CircleRendererComponent& component)
	{
	}

    template<>
    void Level::OnComponentAdded<TagComponent>(Entity entity, TagComponent& component)
    {
    }

    template<>
    void Level::OnComponentAdded<NativeScriptComponent>(Entity entity, NativeScriptComponent& component)
    {
    }

	template<>
	void Level::OnComponentAdded<Rigidbody2DComponent>(Entity entity, Rigidbody2DComponent& component)
	{
	}

	template<>
	void Level::OnComponentAdded<BoxCollider2DComponent>(Entity entity, BoxCollider2DComponent& component)
	{
	}

	template<>
	void Level::OnComponentAdded<CircleCollider2DComponent>(Entity entity, CircleCollider2DComponent& component)
	{

	}

	template<>
	void Level::OnComponentAdded<StaticMeshComponent>(Entity entity, StaticMeshComponent& component)
	{
		component.Mesh = Model(component.Path.string());
	}
}
