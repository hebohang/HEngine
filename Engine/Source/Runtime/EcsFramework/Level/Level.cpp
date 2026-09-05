#include "Hepch.h"

#include "Runtime/Audio/AudioEngine.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/System/SystemGroup.h"
#include "Runtime/EcsFramework/System/SystemLifecycle.h"
#include "Runtime/EcsFramework/System/AI/BehaviorTreeSystem.h"
#include "Runtime/EcsFramework/System/Script/PythonScriptSystem.h"
#include "Runtime/Graphics/Renderer2D.h"
#include "Runtime/Graphics/Mesh/MeshLoader.h"
#include "Runtime/Graphics/Mesh/MeshRuntimeResource.h"
#include "Runtime/EcsFramework/Serializer/PrefabSerializer.h"
#include "Runtime/Utils/MathUtils/MathUtils.h"

#include <glm/glm.hpp>
#include <box2d/b2_world.h>
#include <box2d/b2_body.h>
#include <box2d/b2_fixture.h>
#include <box2d/b2_polygon_shape.h>
#include <box2d/b2_circle_shape.h>

#include <exception>
#include <stdexcept>
#include <unordered_set>

namespace HEngine
{
	Level::Level()
	{
		mRenderPrimitiveCollector = CreateScope<RenderPrimitiveCollector>();

		if (ModeManager::b3DMode)
		{
			mSystems.emplace_back(CreateScope<PhysicSystem3D>(this));
			mSystems.emplace_back(CreateScope<CharacterMovementSystem>(this));
			mSystems.emplace_back(CreateScope<NavPathDebugSystem>(this));
			mSystems.emplace_back(CreateScope<BehaviorTreeSystem>(this));
			mSystems.emplace_back(CreateScope<AnimationSystem>(this));
			mSystems.emplace_back(CreateScope<CameraFollowSystem>(this));
			mSystems.emplace_back(CreateScope<CameraDebugDrawSystem>(this));
			mSystems.emplace_back(CreateScope<RenderSystem3D>(this));
			mSystems.emplace_back(CreateScope<PythonScriptSystem>(this));
			mSystems.emplace_back(CreateScope<AudioScriptSystem>(this));
#if WITH_EDITOR
			mSystems.emplace_back(CreateScope<EmbreeSystem>(this));
#endif
		}
		else
		{
			mSystems.emplace_back(CreateScope<PhysicSystem2D>(this));
			mSystems.emplace_back(CreateScope<NativeScriptSystem>(this));
			mSystems.emplace_back(CreateScope<AudioScriptSystem>(this));
			mSystems.emplace_back(CreateScope<RenderSystem2D>(this));
		}
	}

	Level::~Level()
	{
		if (mRuntimeSystemCount != 0)
		{
			try
			{
				OnRuntimeStop();
			}
			catch (const std::exception& exception)
			{
				Log::WriteFallbackError(
					"Level destructor runtime cleanup failed.",
					exception.what());
			}
			catch (...)
			{
				Log::WriteFallbackError(
					"Level destructor runtime cleanup failed.",
					"unknown exception");
			}
		}

		const auto scriptView = mRegistry.view<IDComponent, PythonScriptComponent>();
		for (const EntityHandle handle : scriptView)
		{
			const UUID owner = scriptView.get<IDComponent>(handle).ID;
			PythonScriptSystem::OnScriptComponentRemoving(this, owner);
		}
	}

		namespace
		{
			void ReportRuntimeLifecycleFailure(
				std::string_view context,
				const std::exception_ptr& failure) noexcept
			{
				try
				{
					std::rethrow_exception(failure);
				}
				catch (const std::exception& error)
				{
					Log::WriteFallbackError(context, error.what());
				}
				catch (...)
				{
					Log::WriteFallbackError(context, "unknown exception");
				}
			}

			static glm::mat4 ComposeTransform(const glm::vec3& translation, const glm::vec3& rotation, const glm::vec3& scale)
			{
				return glm::translate(glm::mat4(1.0f), translation)
					* glm::toMat4(glm::quat(rotation))
					* glm::scale(glm::mat4(1.0f), scale);
			}

			static Entity TryResolveEntityByUuidString(Level* level, const std::string& uuidString)
			{
				if (!level || uuidString.empty())
					return {};

				try
				{
					return level->TryGetEntityByUUID(UUID(uuidString));
				}
				catch (...)
				{
					return {};
				}
			}

			static Ref<Mesh> CloneMeshInstance(const Ref<Mesh>& sourceMesh)
			{
				if (!sourceMesh)
					return nullptr;

				Ref<Mesh> clonedMesh = CreateRef<Mesh>();
				*clonedMesh = *sourceMesh;

				clonedMesh->mAnimator.ClearAnimation();
				if (clonedMesh->bAnimated)
				{
				clonedMesh->mAnimator = Animator(&clonedMesh->mAnimation);
			}

			clonedMesh->bUseExternalBoneMatrices = false;
			clonedMesh->mExternalBoneMatrices.clear();
			clonedMesh->bPlayAnim = false;
			clonedMesh->bStopAnim = false;
			clonedMesh->mAnimPlaySpeed = 1.0f;
			return clonedMesh;
		}
	}

	template<Component... C>
	static void CopyComponent(Registry& dst, Registry& src, const std::unordered_map<UUID, EntityHandle>& enttMap)
	{
		([&]()
			{
				auto view = src.view<C>();
				for (auto srcEntity : view)
				{
					const auto destination = enttMap.find(src.get<IDComponent>(srcEntity).ID);
					if (destination == enttMap.end())
						continue;
					EntityHandle dstEntity = destination->second;

					auto& srcComponent = src.get<C>(srcEntity);
					dst.emplace<C>(dstEntity, srcComponent);
				}
			}(), ...);
	}

	template<Component... C>
	static void CopyComponent(ComponentGroup<C...>, Registry& dst, Registry& src, const std::unordered_map<UUID, EntityHandle>& enttMap)
	{
		CopyComponent<C...>(dst, src, enttMap);
	}

	template<Component... C>
	static void CopyComponentIfExists(Entity dst, Entity src)
	{
		([&]()
			{
				if (src.HasComponent<C>())
				{
					auto componentCopy = src.GetComponent<C>();
					if constexpr (std::is_same_v<C, MeshComponent>)
						componentCopy.mMesh = CloneMeshInstance(componentCopy.mMesh);
					dst.AddOrReplaceComponent<C>(componentCopy);
				}
			}(), ...);
	}

	template<Component... C>
	static void CopyComponentIfExists(ComponentGroup<C...>, Entity dst, Entity src)
	{
		CopyComponentIfExists<C...>(dst, src);
	}

	Ref<Level> Level::Copy(Ref<Level> scene, bool bForRuntime)
	{
		Ref<Level> newScene = CreateRef<Level>();

		std::unordered_map<UUID, EntityHandle> enttMap;
		auto& srcSceneRegistry = scene->mRegistry;
		auto& dstSceneRegistry = newScene->mRegistry;
		auto idView = srcSceneRegistry.view<IDComponent>();
		for (auto e : idView)
		{
#if WITH_EDITOR
			if (bForRuntime && srcSceneRegistry.all_of<SceneFolderComponent>(e))
				continue;
#else
			(void)bForRuntime;
#endif
			UUID uuid = srcSceneRegistry.get<IDComponent>(e).ID;
			const auto& name = srcSceneRegistry.get<TagComponent>(e).Tag;
			Entity newEntity = newScene->CreateEntityWithoutTrans(uuid, name);
			enttMap[uuid] = newEntity;
		}

		CopyComponent(ComponentsCopied{}, dstSceneRegistry, srcSceneRegistry, enttMap);

#if WITH_EDITOR
		if (bForRuntime)
		{
			auto editorStateView = dstSceneRegistry.view<EditorEntityStateComponent>();
			std::vector<EntityHandle> editorStateEntities(editorStateView.begin(), editorStateView.end());
			for (EntityHandle entity : editorStateEntities)
				dstSceneRegistry.remove<EditorEntityStateComponent>(entity);
		}
#endif

		auto meshView = dstSceneRegistry.view<MeshComponent>();
		for (auto dstEntity : meshView)
		{
			auto& meshComponent = meshView.get<MeshComponent>(dstEntity);
			meshComponent.mMesh = CloneMeshInstance(meshComponent.mMesh);

			RuntimeResourceRegistry& resourceRegistry = RuntimeResourceRegistry::GetInstance();
			if (meshComponent.Path != "None" && resourceRegistry.IsInitialized())
			{
				if (!newScene->mRuntimeResources)
					newScene->mRuntimeResources = resourceRegistry.CreateGroup();
				(void)MeshRuntimeResource::AcquirePrototype(
					meshComponent.Path,
					newScene->mRuntimeResources);
			}
		}

			return newScene;
		}

	void Level::ChangeDimMode()
	{
		if (mRuntimeSystemCount != 0)
			throw std::logic_error("Cannot change dimension mode while runtime systems are active.");

		int nowDimMode = ModeManager::b3DMode;
		if (nowDimMode)
		{
			mSystems.clear();
			mSystems.emplace_back(CreateScope<PhysicSystem3D>(this));
			mSystems.emplace_back(CreateScope<CharacterMovementSystem>(this));
			mSystems.emplace_back(CreateScope<NavPathDebugSystem>(this));
			mSystems.emplace_back(CreateScope<BehaviorTreeSystem>(this));
			mSystems.emplace_back(CreateScope<AnimationSystem>(this));
			mSystems.emplace_back(CreateScope<CameraFollowSystem>(this));
			mSystems.emplace_back(CreateScope<CameraDebugDrawSystem>(this));
			mSystems.emplace_back(CreateScope<RenderSystem3D>(this));
			mSystems.emplace_back(CreateScope<PythonScriptSystem>(this));
			mSystems.emplace_back(CreateScope<AudioScriptSystem>(this));
#if WITH_EDITOR
			mSystems.emplace_back(CreateScope<EmbreeSystem>(this));
#endif
		}
		else
		{
			mSystems.clear();
			mSystems.emplace_back(CreateScope<PhysicSystem2D>(this));
			mSystems.emplace_back(CreateScope<NativeScriptSystem>(this));
			mSystems.emplace_back(CreateScope<AudioScriptSystem>(this));
			mSystems.emplace_back(CreateScope<RenderSystem2D>(this));
		}
	}

	Entity Level::CreateEntity(const std::string& name)
	{
		return CreateEntity(UUID(), name);
	}

	Entity Level::CreateEntity(UUID uuid, const std::string& name)
	{
		Entity entity = { mRegistry.create(), this };
		entity.AddComponent<IDComponent>(uuid);
		entity.AddComponent<TransformComponent>();
		auto& tag = entity.AddComponent<TagComponent>();
		tag.Tag = name.empty() ? "Entity" : name;
#if WITH_EDITOR
		InvalidateEditorEntityStateCache();
#endif
		return entity;
	}

	Entity Level::CreateEntityWithoutTrans(UUID uuid, const std::string& name)
	{
		Entity entity = { mRegistry.create(), this };
		entity.AddComponent<IDComponent>(uuid);
		auto& tag = entity.AddComponent<TagComponent>();
		tag.Tag = name.empty() ? "Entity" : name;
#if WITH_EDITOR
		InvalidateEditorEntityStateCache();
#endif
		return entity;
	}

	void Level::DestroyEntity(Entity entity)
	{
		if (!entity)
			return;
		if (entity.GetLevel() != this)
			throw std::invalid_argument("Cannot destroy an entity owned by another level.");

#if WITH_EDITOR
		if (entity.HasComponent<SceneFolderComponent>())
		{
			Entity folderParent = GetEntityFolder(entity);
			for (Entity child : GetFolderChildren(entity))
				SetEntityFolder(child, folderParent);
		}
#endif

		for (Entity child : GetChildren(entity))
			DestroyEntity(child);
		if (!entity)
			return;

#if WITH_EDITOR
		if (auto* embreeSystem = TryGetSystem<EmbreeSystem>())
			embreeSystem->OnEntityRemoved(entity.UUID());
#endif

		if (entity.HasComponent<PythonScriptComponent>())
			entity.RemoveComponent<PythonScriptComponent>();

		if (entity)
		{
			mRegistry.destroy(entity);
#if WITH_EDITOR
			InvalidateEditorEntityStateCache();
#endif
		}
	}

	void Level::OnRuntimeStart()
	{
		if (mRuntimeSystemCount != 0)
			throw std::logic_error("Runtime systems are already active.");

		SetRuntimePaused(false);

		// Runtime startup touches script state and RmlUi documents, both of which must
		// stay on the main thread for stable PIE behavior.
		SystemLifecycle::Start(
			mSystems.size(),
			mRuntimeSystemCount,
			[this](size_t systemIndex)
			{
				mSystems[systemIndex]->OnRuntiemStart();
			},
			[this](size_t systemIndex)
			{
				mSystems[systemIndex]->OnRuntimeStop();
			},
			[this]
			{
				SetRuntimePaused(false);
			},
			ReportRuntimeLifecycleFailure);
	}

	void Level::OnRuntimeStop()
	{
		SystemLifecycle::Stop(
			mRuntimeSystemCount,
			[this](size_t systemIndex)
			{
				mSystems[systemIndex]->OnRuntimeStop();
			},
			[this]
			{
				SetRuntimePaused(false);
			},
			ReportRuntimeLifecycleFailure);
	}

	void Level::SetRuntimePaused(bool bPaused)
	{
		mRuntimePaused = bPaused;
		AudioEngine::GetInstance().SetGameplayPaused(bPaused);
	}

	void Level::OnUpdateRuntime(Timestep ts)
	{
		HE_PROFILE;
		mRenderPrimitiveCollector->BeginFrame();
		Renderer2D::CollectorScope collectorScope(*mRenderPrimitiveCollector);

		// TODO: uniform data bug
		//std::for_each(std::execution::par, mSystems.begin(), mSystems.end(), [&ts](auto& system) {
		//	system->OnUpdateRuntime(ts);
		//	});

		try
		{
			const bool bPausedAtFrameStart = mRuntimePaused;
			for (auto& system : mSystems)
			{
				if (bPausedAtFrameStart && !system->ShouldUpdateWhenRuntimePaused())
					continue;

				HE_PROFILE_TRANSIENT_SECTION(typeid(*system).name());
				system->OnUpdateRuntime(ts);
			}
			mRenderPrimitiveCollector->EndFrame();
		}
		catch (...)
		{
			mRenderPrimitiveCollector->CancelFrame();
			throw;
		}
	}

	#if WITH_EDITOR
	void Level::OnUpdateEditor(Timestep ts, EditorCamera& camera)
	{
		HE_PROFILE;
		RebuildEditorEntityStateCache();
		mRenderPrimitiveCollector->BeginFrame();
		Renderer2D::CollectorScope collectorScope(*mRenderPrimitiveCollector);

		try
		{
			PhysicSystem3D* deferredPhysicsSystem = nullptr;
			for (auto& system : mSystems)
			{
				if (auto* physicsSystem = dynamic_cast<PhysicSystem3D*>(system.get()))
				{
					deferredPhysicsSystem = physicsSystem;
					continue;
				}

				// Physics debug geometry used to execute after rendering. It must now
				// be collected immediately before 3D extraction so it is part of the
				// immutable RenderWorld submitted for this frame.
				if (dynamic_cast<RenderSystem3D*>(system.get()) && deferredPhysicsSystem)
				{
					HE_PROFILE_TRANSIENT_SECTION(typeid(*deferredPhysicsSystem).name());
					deferredPhysicsSystem->OnUpdateEditor(ts, camera);
					deferredPhysicsSystem = nullptr;
				}

				HE_PROFILE_TRANSIENT_SECTION(typeid(*system).name());
				system->OnUpdateEditor(ts, camera);
			}

			if (deferredPhysicsSystem)
			{
				HE_PROFILE_TRANSIENT_SECTION(typeid(*deferredPhysicsSystem).name());
				deferredPhysicsSystem->OnUpdateEditor(ts, camera);
			}

			mRenderPrimitiveCollector->EndFrame();
		}
		catch (...)
		{
			mRenderPrimitiveCollector->CancelFrame();
			throw;
		}
	}

	bool Level::RequiresContinuousEditorUpdate() const
	{
		for (const auto& system : mSystems)
		{
			if (system->RequiresContinuousEditorUpdate())
				return true;
		}
		return false;
	}
#endif

	void Level::OnImGuiRender()
	{
		for (auto& system : mSystems)
		{
			system->OnImGuiRender();
		}
	}

#if WITH_EDITOR
	void Level::OnImGuiRenderEditor()
	{
		for (auto& system : mSystems)
		{
			system->OnImGuiRenderEditor();
		}
	}
#endif

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

	Entity Level::DuplicateEntity(Entity entity, bool bDuplicateTransformChildren)
	{
		if (!entity)
			return {};

		Entity newEntity = CreateEntity(entity.GetName());

		// CreateEntity already assigned a fresh ID and copied the display name.
		// Only copy persisted payload components so Ctrl+D cannot duplicate UUIDs.
		CopyComponentIfExists(ComponentsCopied{}, newEntity, entity);

		if (newEntity.HasComponent<TransformComponent>())
		{
			auto& transform = newEntity.GetComponent<TransformComponent>();
			Entity parent = GetParent(entity);
			transform.ParentUuid = parent ? parent.UUID().str() : std::string();
#if WITH_EDITOR
			InvalidateEditorEntityStateCache();
#endif
		}

		if (bDuplicateTransformChildren)
		{
			for (Entity child : GetChildren(entity))
			{
				Entity duplicatedChild = DuplicateEntity(child, true);
				if (duplicatedChild)
					SetEntityParent(duplicatedChild, newEntity, false);
			}
		}

		return newEntity;
	}

	bool Level::SetEntityParent(Entity child, Entity parent, bool bKeepWorldTransform)
	{
		if (!child || !child.HasComponent<TransformComponent>())
			return false;

		if (parent && (!parent.HasComponent<TransformComponent>() || parent.GetLevel() != this))
			return false;

		if (child == parent || IsEntityDescendantOf(parent, child))
			return false;

		Entity currentParent = GetParent(child);
		if ((currentParent && parent && currentParent == parent) || (!currentParent && !parent))
			return false;

		const glm::mat4 worldTransform = bKeepWorldTransform ? GetWorldTransform(child) : glm::mat4(1.0f);

		auto& transform = child.GetComponent<TransformComponent>();
		transform.ParentUuid = parent ? parent.UUID().str() : std::string();
#if WITH_EDITOR
		InvalidateEditorEntityStateCache();
#endif

		if (bKeepWorldTransform)
			return SetWorldTransform(child, worldTransform);

		return true;
	}

	Entity Level::GetParent(Entity entity)
	{
		if (!entity || !entity.HasComponent<TransformComponent>())
			return {};

		const std::string& parentUuid = entity.GetComponent<TransformComponent>().ParentUuid;
		if (parentUuid.empty())
			return {};

#if WITH_EDITOR
		if (mEditorEntityStateCacheBuilt)
		{
			const auto cachedParent = mEditorEntityByUuid.find(parentUuid);
			if (cachedParent != mEditorEntityByUuid.end() && cachedParent->second != static_cast<EntityHandle>(entity) &&
				mRegistry.valid(cachedParent->second) && mRegistry.all_of<IDComponent>(cachedParent->second) &&
				mRegistry.get<IDComponent>(cachedParent->second).ID.str() == parentUuid)
			{
				return Entity{ cachedParent->second, this };
			}
		}
#endif

		Entity parent = TryResolveEntityByUuidString(this, parentUuid);
		if (!parent || parent == entity)
			return {};

		return parent;
	}

#if WITH_EDITOR
	bool Level::SetEntityFolder(Entity entity, Entity folder)
	{
		if (!entity || entity.GetLevel() != this)
			return false;
		if (folder && (folder.GetLevel() != this || !folder.HasComponent<SceneFolderComponent>()))
			return false;
		if (entity == folder || IsEntityInFolderDescendantOf(folder, entity) || IsEntityDescendantOf(folder, entity))
			return false;

		auto& editorState = entity.AddOrReplaceComponent<EditorEntityStateComponent>(
			entity.HasComponent<EditorEntityStateComponent>()
				? entity.GetComponent<EditorEntityStateComponent>()
				: EditorEntityStateComponent{});
		const std::string folderUuid = folder ? folder.UUID().str() : std::string();
		if (editorState.FolderUuid == folderUuid)
			return false;

		editorState.FolderUuid = folderUuid;
		InvalidateEditorEntityStateCache();
		return true;
	}

	Entity Level::GetEntityFolder(Entity entity)
	{
		if (!entity || entity.GetLevel() != this || !entity.HasComponent<EditorEntityStateComponent>())
			return {};

		const std::string& folderUuid = entity.GetComponent<EditorEntityStateComponent>().FolderUuid;
		if (folderUuid.empty())
			return {};

		Entity folder = TryResolveEntityByUuidString(this, folderUuid);
		if (!folder || folder == entity || !folder.HasComponent<SceneFolderComponent>())
			return {};
		return folder;
	}

	std::vector<Entity> Level::GetFolderChildren(Entity folder)
	{
		std::vector<Entity> children;
		if (!folder || folder.GetLevel() != this || !folder.HasComponent<SceneFolderComponent>())
			return children;

		const std::string folderUuid = folder.UUID().str();
		auto view = mRegistry.view<EditorEntityStateComponent>();
		for (auto handle : view)
		{
			if (view.get<EditorEntityStateComponent>(handle).FolderUuid == folderUuid)
				children.emplace_back(handle, this);
		}
		return children;
	}

	bool Level::IsEntityInFolderDescendantOf(Entity entity, Entity potentialAncestor)
	{
		if (!entity || !potentialAncestor || entity == potentialAncestor)
			return false;

		Entity current = GetEntityFolder(entity);
		for (int depth = 0; current && depth < 256; ++depth)
		{
			if (current == potentialAncestor)
				return true;
			current = GetEntityFolder(current);
		}
		return false;
	}

	void Level::MigrateLegacySceneFolderParents()
	{
		std::vector<std::pair<Entity, Entity>> legacyFolderParents;
		auto transformView = mRegistry.view<TransformComponent>();
		for (auto handle : transformView)
		{
			Entity entity{ handle, this };
			Entity parent = GetParent(entity);
			if (parent && parent.HasComponent<SceneFolderComponent>())
				legacyFolderParents.emplace_back(entity, parent);
		}

		for (auto& [entity, folder] : legacyFolderParents)
		{
			SetEntityFolder(entity, folder);
			SetEntityParent(entity, {}, true);
		}
	}

	void Level::RebuildEditorEntityStateCache()
	{
		if (mEditorEntityStateCacheBuilt && !mEditorEntityStateCacheDirty)
			return;

		HE_PROFILE;
		mEditorEffectiveVisibility.clear();
		mEditorEffectiveLock.clear();

		auto idView = mRegistry.view<IDComponent>();
		mEditorEntityByUuid.clear();
		mEditorEntityByUuid.reserve(idView.size());
		mEditorEffectiveVisibility.reserve(idView.size());
		mEditorEffectiveLock.reserve(idView.size());
		for (auto handle : idView)
			mEditorEntityByUuid.emplace(idView.get<IDComponent>(handle).ID.str(), handle);

		const size_t maximumDepth = idView.size() * 2 + 1;
		for (auto handle : idView)
		{
			bool bVisible = true;
			bool bLocked = false;
			std::vector<EntityHandle> pending{ handle };
			std::unordered_set<uint32_t> visited;
			for (size_t depth = 0; !pending.empty() && depth < maximumDepth; ++depth)
			{
				const EntityHandle current = pending.back();
				pending.pop_back();
				if (!visited.emplace(static_cast<uint32_t>(current)).second)
					continue;

				if (mRegistry.all_of<EditorEntityStateComponent>(current))
				{
					const auto& editorState = mRegistry.get<EditorEntityStateComponent>(current);
					bVisible = bVisible && editorState.bVisible;
					bLocked = bLocked || editorState.bLocked;
					if (!editorState.FolderUuid.empty())
					{
						const auto folderIt = mEditorEntityByUuid.find(editorState.FolderUuid);
						if (folderIt != mEditorEntityByUuid.end() && folderIt->second != current)
							pending.push_back(folderIt->second);
					}
				}

				if (mRegistry.all_of<TransformComponent>(current))
				{
					const std::string& parentUuid = mRegistry.get<TransformComponent>(current).ParentUuid;
					const auto parentIt = mEditorEntityByUuid.find(parentUuid);
					if (parentIt != mEditorEntityByUuid.end() && parentIt->second != current)
						pending.push_back(parentIt->second);
				}
			}

			mEditorEffectiveVisibility.emplace(static_cast<uint32_t>(handle), bVisible);
			mEditorEffectiveLock.emplace(static_cast<uint32_t>(handle), bLocked);
		}
		mEditorEntityStateCacheBuilt = true;
		mEditorEntityStateCacheDirty = false;
	}

	void Level::InvalidateEditorEntityStateCache() noexcept
	{
		mEditorEntityStateCacheBuilt = false;
		mEditorEntityStateCacheDirty = true;
	}

	bool Level::IsEntityVisibleInEditor(Entity entity)
	{
		if (entity && entity.GetLevel() == this &&
			(!mEditorEntityStateCacheBuilt || mEditorEntityStateCacheDirty))
		{
			RebuildEditorEntityStateCache();
		}

		if (entity && entity.GetLevel() == this && mEditorEntityStateCacheBuilt)
		{
			const auto cached = mEditorEffectiveVisibility.find(static_cast<uint32_t>(entity));
			if (cached != mEditorEffectiveVisibility.end())
				return cached->second;
		}

		return !entity || !entity.HasComponent<EditorEntityStateComponent>() ||
			entity.GetComponent<EditorEntityStateComponent>().bVisible;
	}

	bool Level::IsEntityLockedInEditor(Entity entity)
	{
		if (entity && entity.GetLevel() == this &&
			(!mEditorEntityStateCacheBuilt || mEditorEntityStateCacheDirty))
		{
			RebuildEditorEntityStateCache();
		}

		if (entity && entity.GetLevel() == this && mEditorEntityStateCacheBuilt)
		{
			const auto cached = mEditorEffectiveLock.find(static_cast<uint32_t>(entity));
			if (cached != mEditorEffectiveLock.end())
				return cached->second;
		}

		return entity && entity.HasComponent<EditorEntityStateComponent>() &&
			entity.GetComponent<EditorEntityStateComponent>().bLocked;
	}
#endif

	std::vector<Entity> Level::GetChildren(Entity parent)
	{
		std::vector<Entity> children;
		if (!parent)
			return children;

		const std::string parentUuid = parent.UUID().str();
		auto view = mRegistry.view<TransformComponent>();
		for (auto handle : view)
		{
			const auto& transform = view.get<TransformComponent>(handle);
			if (transform.ParentUuid == parentUuid)
				children.emplace_back(handle, this);
		}

		return children;
	}

	bool Level::HasChildren(Entity parent)
	{
		return !GetChildren(parent).empty();
	}

	bool Level::IsEntityDescendantOf(Entity entity, Entity potentialAncestor)
	{
		if (!entity || !potentialAncestor || entity == potentialAncestor)
			return false;

		Entity current = GetParent(entity);
		for (int depth = 0; current && depth < 256; ++depth)
		{
			if (current == potentialAncestor)
				return true;

			current = GetParent(current);
		}

		return false;
	}

	glm::mat4 Level::GetWorldTransform(Entity entity)
	{
		if (!entity || !entity.HasComponent<TransformComponent>())
			return glm::mat4(1.0f);

		glm::mat4 worldTransform = entity.GetComponent<TransformComponent>().GetTransform();
		Entity parent = GetParent(entity);
		for (int depth = 0; parent && depth < 256; ++depth)
		{
			worldTransform = parent.GetComponent<TransformComponent>().GetTransform() * worldTransform;
			parent = GetParent(parent);
		}

		return worldTransform;
	}

	glm::vec3 Level::GetWorldTranslation(Entity entity)
	{
		if (entity && entity.HasComponent<TransformComponent>() && !GetParent(entity))
			return entity.GetComponent<TransformComponent>().Translation;

		return glm::vec3(GetWorldTransform(entity)[3]);
	}

	glm::vec3 Level::GetWorldRotation(Entity entity)
	{
		if (entity && entity.HasComponent<TransformComponent>() && !GetParent(entity))
			return entity.GetComponent<TransformComponent>().Rotation;

		glm::vec3 translation(0.0f);
		glm::vec3 rotation(0.0f);
		glm::vec3 scale(1.0f);
		if (GetWorldTransformComponents(entity, translation, rotation, scale))
			return rotation;
		return glm::vec3(0.0f);
	}

	glm::vec3 Level::GetWorldScale(Entity entity)
	{
		if (entity && entity.HasComponent<TransformComponent>() && !GetParent(entity))
			return entity.GetComponent<TransformComponent>().Scale;

		glm::vec3 translation(0.0f);
		glm::vec3 rotation(0.0f);
		glm::vec3 scale(1.0f);
		if (GetWorldTransformComponents(entity, translation, rotation, scale))
			return scale;
		return glm::vec3(1.0f);
	}

	bool Level::GetWorldTransformComponents(Entity entity, glm::vec3& outTranslation, glm::vec3& outRotation, glm::vec3& outScale)
	{
		if (!entity || !entity.HasComponent<TransformComponent>())
			return false;

		if (!GetParent(entity))
		{
			const auto& transform = entity.GetComponent<TransformComponent>();
			outTranslation = transform.Translation;
			outRotation = transform.Rotation;
			outScale = transform.Scale;
			return true;
		}

		return Math::DecomposeTransform(GetWorldTransform(entity), outTranslation, outRotation, outScale);
	}

	bool Level::SetWorldTransform(Entity entity, const glm::mat4& worldTransform)
	{
		if (!entity || !entity.HasComponent<TransformComponent>())
			return false;

		glm::mat4 localTransform = worldTransform;
		Entity parent = GetParent(entity);
		if (parent)
			localTransform = glm::inverse(GetWorldTransform(parent)) * worldTransform;

		glm::vec3 translation(0.0f);
		glm::vec3 rotation(0.0f);
		glm::vec3 scale(1.0f);
		if (!Math::DecomposeTransform(localTransform, translation, rotation, scale))
			return false;

		auto& transform = entity.GetComponent<TransformComponent>();
		transform.Translation = translation;
		transform.Rotation = rotation;
		transform.Scale = scale;
		return true;
	}

	bool Level::SetWorldTransformComponents(Entity entity, const glm::vec3& translation, const glm::vec3& rotation, const glm::vec3& scale)
	{
		if (entity && entity.HasComponent<TransformComponent>() && !GetParent(entity))
		{
			auto& transform = entity.GetComponent<TransformComponent>();
			transform.Translation = translation;
			transform.Rotation = rotation;
			transform.Scale = scale;
			return true;
		}

		return SetWorldTransform(entity, ComposeTransform(translation, rotation, scale));
	}

	bool Level::SetWorldTranslation(Entity entity, const glm::vec3& translation)
	{
		if (entity && entity.HasComponent<TransformComponent>() && !GetParent(entity))
		{
			entity.GetComponent<TransformComponent>().Translation = translation;
			return true;
		}

		glm::vec3 currentTranslation(0.0f);
		glm::vec3 currentRotation(0.0f);
		glm::vec3 currentScale(1.0f);
		if (!GetWorldTransformComponents(entity, currentTranslation, currentRotation, currentScale))
			return false;

		return SetWorldTransformComponents(entity, translation, currentRotation, currentScale);
	}

	bool Level::SetWorldRotation(Entity entity, const glm::vec3& rotation)
	{
		if (entity && entity.HasComponent<TransformComponent>() && !GetParent(entity))
		{
			entity.GetComponent<TransformComponent>().Rotation = rotation;
			return true;
		}

		glm::vec3 currentTranslation(0.0f);
		glm::vec3 currentRotation(0.0f);
		glm::vec3 currentScale(1.0f);
		if (!GetWorldTransformComponents(entity, currentTranslation, currentRotation, currentScale))
			return false;

		return SetWorldTransformComponents(entity, currentTranslation, rotation, currentScale);
	}

    Entity Level::InstantiatePrefab(const std::string& prefabPath)
    {
        return PrefabSerializer::InstantiatePrefab(this, prefabPath);
    }

    Entity Level::InstantiatePrefab(const std::string& prefabPath, const glm::vec3& position)
    {
        return PrefabSerializer::InstantiatePrefab(this, prefabPath, position);
    }

    Entity Level::InstantiatePrefab(const std::string& prefabPath, const glm::vec3& position, const glm::vec3& rotation)
    {
        return PrefabSerializer::InstantiatePrefab(this, prefabPath, position, rotation);
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

	Entity Level::TryGetEntityByUUID(const UUID& uuid)
	{
#if WITH_EDITOR
		if (mEditorEntityStateCacheBuilt)
		{
			const auto cached = mEditorEntityByUuid.find(uuid.str());
			if (cached != mEditorEntityByUuid.end() && mRegistry.valid(cached->second) &&
				mRegistry.all_of<IDComponent>(cached->second) && mRegistry.get<IDComponent>(cached->second).ID == uuid)
			{
				return Entity{ cached->second, this };
			}
		}
#endif
		auto view = mRegistry.view<IDComponent>();
		for (auto e : view)
		{
			const auto& idComp = view.get<IDComponent>(e);
			if (idComp.ID == uuid)
				return Entity{ e, this };
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
		if (!component.FixedAspectRatio &&
			ConfigManager::mViewportSize.x > 0 && ConfigManager::mViewportSize.y > 0)
			component.Camera.SetViewportSize(ConfigManager::mViewportSize.x, ConfigManager::mViewportSize.y);
	}

	template<>
	void Level::OnComponentAdded<FollowCameraComponent>(Entity entity, FollowCameraComponent& component)
	{
		component.bRuntimeInitialized = false;
		component.RuntimeMissingTargetWarnCooldown = 0.0f;
		component.RuntimeMissingTransformWarnCooldown = 0.0f;
	}

	template<>
	void Level::OnComponentAdded<SpringArmComponent>(Entity entity, SpringArmComponent& component)
	{
	}

	template<>
	void Level::OnComponentAdded<CharacterMovementComponent>(Entity entity, CharacterMovementComponent& component)
	{
		component.RuntimeCharacter = nullptr;
		component.RuntimeVelocity = glm::vec3(0.0f);
		component.RuntimeDesiredVelocity = glm::vec3(0.0f);
		component.RuntimeHasExternalVelocity = false;
		component.RuntimeAnimDirection = 0.0f;
		component.RuntimeLastInputYaw = 0.0f;
		component.RuntimeHasLastInputYaw = false;
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
	void Level::OnComponentAdded<PythonScriptComponent>(Entity entity, PythonScriptComponent& component)
	{
		PythonScriptSystem::OnScriptComponentAdded(entity, component);
	}

	void Level::OnPythonScriptComponentRemoving(Entity& entity) noexcept
	{
		PythonScriptSystem::OnScriptComponentRemoving(this, entity.UUID());
	}

	void Level::OnMeshComponentRemoving(Entity& entity) noexcept
	{
#if WITH_EDITOR
		if (auto* embreeSystem = TryGetSystem<EmbreeSystem>())
			embreeSystem->OnEntityRemoved(entity.UUID());
#else
		(void)entity;
#endif
	}

	template<>
	void Level::OnComponentAdded<Rigidbody2DComponent>(Entity entity, Rigidbody2DComponent& component)
	{
	}

	template<>
	void Level::OnComponentAdded<Rigidbody3DComponent>(Entity entity, Rigidbody3DComponent& component)
	{
		component.RuntimeBody = nullptr;
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
	void Level::OnComponentAdded<CapsuleCollider3DComponent>(Entity entity, CapsuleCollider3DComponent& component)
	{
		component.RuntimeShape = nullptr;
	}

	template<>
	void Level::OnComponentAdded<MeshComponent>(Entity entity, MeshComponent& component)
	{
		if (component.Path != "None" && (!component.mMesh || component.mMesh->mSubMeshes.empty()))
		{
			if (mRuntimeResourceRegistry)
			{
				if (!mRuntimeResourceRegistry->IsInitialized())
					throw std::logic_error("The Level mesh resource registry is unavailable.");
				if (!mRuntimeResourceCatalog || mRuntimeResourceProjectRoot.empty())
					throw std::logic_error("The Level mesh resource context is incomplete.");
				if (!mRuntimeResources)
					mRuntimeResources = mRuntimeResourceRegistry->CreateGroup();
				const Ref<Mesh> prototype = MeshRuntimeResource::AcquirePrototype(
					*mRuntimeResourceRegistry,
					mRuntimeResourceProjectRoot,
					mRuntimeResourceCatalog,
					component.Path,
					mRuntimeResources);
				component.mMesh = CloneMeshInstance(prototype);
				component.ApplyMaterialOverrides();
				return;
			}

			RuntimeResourceRegistry& resourceRegistry = RuntimeResourceRegistry::GetInstance();
			if (resourceRegistry.IsInitialized())
			{
				if (!mRuntimeResources)
					mRuntimeResources = resourceRegistry.CreateGroup();
				const Ref<Mesh> prototype =
					MeshRuntimeResource::AcquirePrototype(component.Path, mRuntimeResources);
				component.mMesh = CloneMeshInstance(prototype);
			}
			else
			{
				Ref<Mesh> prototype = CreateRef<Mesh>();
				MeshLoader::LoadModel(prototype.get(), component.Path);
				component.mMesh = CloneMeshInstance(prototype);
			}
		}
		component.ApplyMaterialOverrides();
	}
}
