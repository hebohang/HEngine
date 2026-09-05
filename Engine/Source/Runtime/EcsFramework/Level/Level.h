#pragma once

#include "Runtime/Core/Timestep.h"
#include "Runtime/Core/UUID.h"
#include "Runtime/Core/Base/Base.h"
#include "Runtime/Camera/EditorCamera.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeResourceRegistry.h"

#include <entt.hpp>
#include <glm/glm.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class b2World;

namespace HEngine
{
    // we can use uint64_t like this
    //enum class EntityHandle : std::uint64_t {};
    using EntityHandle = entt::entity;
    using Registry = entt::basic_registry<EntityHandle>;
}

namespace HEngine
{
    class Entity;
    class RuntimeResourceCatalogSnapshot;
	class MeshComponent;
	class PythonScriptComponent;
	class RenderPrimitiveCollector;

    class Level
    {
    public:
        Level();
        ~Level();

		static Ref<Level> Copy(Ref<Level> other, bool bForRuntime = false);

        void ChangeDimMode();

        // Common entity should have IDComponent TagComponent and TransformComponent
        Entity CreateEntity(const std::string& name = std::string());
        Entity CreateEntity(UUID uuid, const std::string& name = std::string());
        Entity CreateEntityWithoutTrans(UUID uuid, const std::string& name = std::string());

        void DestroyEntity(Entity entity);

		void OnRuntimeStart();
		void OnRuntimeStop();

        void OnUpdateRuntime(Timestep ts);
		void SetRuntimePaused(bool bPaused);
		[[nodiscard]] bool IsRuntimePaused() const { return mRuntimePaused; }
        void OnImGuiRender();
#if WITH_EDITOR
        void OnUpdateEditor(Timestep ts, EditorCamera& camera);
        void OnImGuiRenderEditor();
		[[nodiscard]] bool RequiresContinuousEditorUpdate() const;
#endif
		void OnViewportResize(uint32_t width, uint32_t height);

		Entity DuplicateEntity(Entity entity, bool bDuplicateTransformChildren = true);
		bool SetEntityParent(Entity child, Entity parent, bool bKeepWorldTransform = true);
		Entity GetParent(Entity entity);
		std::vector<Entity> GetChildren(Entity parent);
		bool HasChildren(Entity parent);
		bool IsEntityDescendantOf(Entity entity, Entity potentialAncestor);
#if WITH_EDITOR
		bool IsEntityVisibleInEditor(Entity entity);
		bool IsEntityLockedInEditor(Entity entity);
		bool SetEntityFolder(Entity entity, Entity folder);
		Entity GetEntityFolder(Entity entity);
		std::vector<Entity> GetFolderChildren(Entity folder);
		bool IsEntityInFolderDescendantOf(Entity entity, Entity potentialAncestor);
		void MigrateLegacySceneFolderParents();
		void InvalidateEditorEntityStateCache() noexcept;
#endif
		glm::mat4 GetWorldTransform(Entity entity);
		glm::vec3 GetWorldTranslation(Entity entity);
		glm::vec3 GetWorldRotation(Entity entity);
		glm::vec3 GetWorldScale(Entity entity);
		bool GetWorldTransformComponents(Entity entity, glm::vec3& outTranslation, glm::vec3& outRotation, glm::vec3& outScale);
		bool SetWorldTransform(Entity entity, const glm::mat4& worldTransform);
		bool SetWorldTransformComponents(Entity entity, const glm::vec3& translation, const glm::vec3& rotation, const glm::vec3& scale);
		bool SetWorldTranslation(Entity entity, const glm::vec3& translation);
		bool SetWorldRotation(Entity entity, const glm::vec3& rotation);

        Entity InstantiatePrefab(const std::string& prefabPath);
        Entity InstantiatePrefab(const std::string& prefabPath, const glm::vec3& position);
        Entity InstantiatePrefab(const std::string& prefabPath, const glm::vec3& position, const glm::vec3& rotation);

		Entity GetPrimaryCameraEntity();
		Entity TryGetEntityByUUID(const UUID& uuid);
		template<typename T>
		T* TryGetSystem();

		template<typename... Components>
		auto GetAllEntitiesWith()
		{
			return mRegistry.view<Components...>();
		}
    private:
        template<typename T>
        void OnComponentAdded(Entity entity, T& component);
		void OnMeshComponentRemoving(Entity& entity) noexcept;
		void OnPythonScriptComponentRemoving(Entity& entity) noexcept;
#if WITH_EDITOR
		void RebuildEditorEntityStateCache();
#endif
    private:
		// Explicit context is installed only by staged materialization. It is not
		// copied by Level::Copy, so a cloned Level cannot retain another Level's
		// registry lifetime implicitly. The group is declared last and releases
		// its pins before the context snapshot is destroyed.
		RuntimeResourceRegistry* mRuntimeResourceRegistry = nullptr;
		std::filesystem::path mRuntimeResourceProjectRoot;
		std::shared_ptr<const RuntimeResourceCatalogSnapshot> mRuntimeResourceCatalog;
        RuntimeResourceGroup mRuntimeResources;
    public:
        Registry mRegistry;
    private:
        std::vector<Scope<class System>> mSystems;
		Scope<RenderPrimitiveCollector> mRenderPrimitiveCollector;
		size_t mRuntimeSystemCount = 0;
		bool mRuntimePaused = false;
#if WITH_EDITOR
		std::unordered_map<std::string, EntityHandle> mEditorEntityByUuid;
		std::unordered_map<uint32_t, bool> mEditorEffectiveVisibility;
		std::unordered_map<uint32_t, bool> mEditorEffectiveLock;
		bool mEditorEntityStateCacheBuilt = false;
		bool mEditorEntityStateCacheDirty = true;
#endif

		friend class Entity;
		friend class PrefabSerializer;
		friend class SceneSerializer;
		friend class IncrementalSceneMaterializer;
		friend class SceneHierarchyPanel;
	};

	template<typename T>
	inline T* Level::TryGetSystem()
	{
		for (auto& system : mSystems)
		{
			if (auto* result = dynamic_cast<T*>(system.get()))
				return result;
		}
		return nullptr;
	}
}
