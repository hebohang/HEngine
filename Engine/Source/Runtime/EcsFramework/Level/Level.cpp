#include "hepch.h"

#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/System/SystemGroup.h"
#include "Runtime/Renderer/Renderer2D.h"
#include "Runtime/Renderer/Renderer3D.h"
#include "Runtime/Utils/MathUtils/MathUtils.h"

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
			mSystems.emplace_back(CreateScope<EnvironmentSystem>(this));
			mSystems.emplace_back(CreateScope<PythonScriptSystem>(this));
			mSystems.emplace_back(CreateScope<AudioScriptSystem>(this));
		}
		else
		{
			mSystems.emplace_back(CreateScope<PhysicSystem2D>(this));
			mSystems.emplace_back(CreateScope<NativeScriptSystem>(this));
			mSystems.emplace_back(CreateScope<RenderSystem2D>(this));
			mSystems.emplace_back(CreateScope<EnvironmentSystem>(this));
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
			mSystems.emplace_back(CreateScope<EnvironmentSystem>(this));
		}
		else
		{
			mSystems.clear();
			mSystems.emplace_back(CreateScope<PhysicSystem2D>(this));
			mSystems.emplace_back(CreateScope<NativeScriptSystem>(this));
			mSystems.emplace_back(CreateScope<RenderSystem2D>(this));
			mSystems.emplace_back(CreateScope<EnvironmentSystem>(this));
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
		if (ConfigManager::mViewportSize.x > 0 && ConfigManager::mViewportSize.y > 0)
			component.Camera.SetViewportSize(ConfigManager::mViewportSize.x, ConfigManager::mViewportSize.y);
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
	void Level::OnComponentAdded<MeshComponent>(Entity entity, MeshComponent& component)
	{
	}

	template<>
	void Level::OnComponentAdded<Rigidbody3DComponent>(Entity entity, Rigidbody3DComponent& component)
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

		// sort to obtain eValue[0] <= eValue[1] <= eValue[2] (eVectors with the same order of eValues)
		for (int i = 0; i < 2; i++)
		{
			for (int j = 0; j < 2 - i; j++)
			{
				if (eValues[j] > eValues[j + 1])
				{
					float temp = eValues[j];
					eValues[j] = eValues[j + 1];
					eValues[j + 1] = temp;

					glm::vec3 tempVec = eVectors[j];
					eVectors[j] = eVectors[j + 1];
					eVectors[j + 1] = tempVec;
				}
			}
		}
		Math::SchmidtOrthogonalization(eVectors[2], eVectors[1], eVectors[0]);

		constexpr float infinity = std::numeric_limits<float>::infinity();
		glm::vec3 minExtents(infinity, infinity, infinity);
		glm::vec3 maxExtents(-infinity, -infinity, -infinity);

		for (const glm::vec3& displacement : vertices)
		{
			minExtents.x = std::min(minExtents.x, glm::dot(displacement, eVectors[2]));
			minExtents.y = std::min(minExtents.y, glm::dot(displacement, eVectors[1]));
			minExtents.z = std::min(minExtents.z, glm::dot(displacement, eVectors[0]));

			maxExtents.x = std::max(maxExtents.x, glm::dot(displacement, eVectors[2]));
			maxExtents.y = std::max(maxExtents.y, glm::dot(displacement, eVectors[1]));
			maxExtents.z = std::max(maxExtents.z, glm::dot(displacement, eVectors[0]));
		}

		glm::vec3 halfExtent = (maxExtents - minExtents) / 2.0f;
		glm::vec3 offset = halfExtent + minExtents;
		originPos += offset.x * eVectors[2] + offset.y * eVectors[1] + offset.z * eVectors[0];

#if 0
		// Cov Matrix test
		// from https://blog.csdn.net/qing101hua/article/details/53100112
		std::vector<glm::vec3> testVertices;
		testVertices.emplace_back(3.7, 1.7, 0.0);
		testVertices.emplace_back(4.1, 3.8, 0.0);
		testVertices.emplace_back(4.7, 2.9, 0.0);
		testVertices.emplace_back(5.2, 2.8, 0.0);
		testVertices.emplace_back(6.0, 4.0, 0.0);
		testVertices.emplace_back(6.3, 3.6, 0.0);
		testVertices.emplace_back(9.7, 6.3, 0.0);
		testVertices.emplace_back(10.0, 4.9, 0.0);
		testVertices.emplace_back(11.0, 3.6, 0.0);
		testVertices.emplace_back(12.5, 6.4, 0.0);
		glm::mat3 testCovMat = Math::CalculateCovMatrix(testVertices);

		glm::vec3 eValues;
		glm::mat3 eVectors;
		Math::JacobiSolver(testCovMat, eValues, eVectors);
#endif
	}
}
