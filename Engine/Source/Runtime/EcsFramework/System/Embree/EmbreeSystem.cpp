#include "Hepch.h"

#include "Runtime/EcsFramework/System/Embree/EmbreeSystem.h"

#if WITH_EDITOR
#include "Runtime/EcsFramework/Component/Basic/IDComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Platform/Embree/EditorEmbreeContext.h"

#include <glm/gtc/type_ptr.hpp>

#include <cstring>
#include <exception>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#endif

namespace HEngine
{
#if WITH_EDITOR
	struct EmbreeSystem::EditorState
	{
		struct Binding
		{
			EntityHandle Handle{ entt::null };
			Ref<Mesh> MeshAsset;
			Ref<const MeshEditorPickData> PickData;
			Scope<EditorEmbreeInstance> Instance;
			glm::mat4 WorldTransform{ 1.0f };
		};

		std::unordered_map<UUID, Binding> Bindings;
		std::unordered_map<std::uint32_t, UUID> InstanceOwners;
		std::uint64_t ContextGeneration = 0;
		bool bInitialSynchronizationPending = true;
	};

	namespace
	{
		bool MatricesEqual(const glm::mat4& left, const glm::mat4& right) noexcept
		{
			return std::memcmp(
				glm::value_ptr(left),
				glm::value_ptr(right),
				sizeof(glm::mat4)) == 0;
		}
	}
#endif

	EmbreeSystem::EmbreeSystem(Level* level)
		: System(level)
#if WITH_EDITOR
		, mEditorState(CreateScope<EditorState>())
#endif
	{
	}

	EmbreeSystem::~EmbreeSystem()
	{
#if WITH_EDITOR
		ClearBindings();
		if (EditorEmbreeContext::IsInitialized())
			(void)EditorEmbreeContext::Commit();
#endif
	}

	void EmbreeSystem::OnRuntiemStart()
	{
	}

	void EmbreeSystem::OnUpdateRuntime(Timestep ts)
	{
		(void)ts;
	}

	void EmbreeSystem::OnRuntimeStop()
	{
	}

#if WITH_EDITOR
	void EmbreeSystem::InitializeSharedContext()
	{
		EditorEmbreeContext::Initialize();
	}

	void EmbreeSystem::ResetSharedContext()
	{
		EditorEmbreeContext::Reset();
	}

	void EmbreeSystem::ShutdownSharedContext() noexcept
	{
		EditorEmbreeContext::Shutdown();
	}

	bool EmbreeSystem::IsSharedContextInitialized() noexcept
	{
		return EditorEmbreeContext::IsInitialized();
	}

	void EmbreeSystem::RemoveBinding(const UUID& entityID) noexcept
	{
		if (!mEditorState)
			return;

		const auto binding = mEditorState->Bindings.find(entityID);
		if (binding == mEditorState->Bindings.end())
			return;

		if (binding->second.Instance)
			mEditorState->InstanceOwners.erase(binding->second.Instance->GetInstanceID());
		mEditorState->Bindings.erase(binding);
	}

	void EmbreeSystem::ClearBindings() noexcept
	{
		if (!mEditorState)
			return;

		mEditorState->InstanceOwners.clear();
		mEditorState->Bindings.clear();
	}

	void EmbreeSystem::OnEntityRemoved(const UUID& entityID) noexcept
	{
		RemoveBinding(entityID);
		if (EditorEmbreeContext::IsInitialized())
			(void)EditorEmbreeContext::Commit();
	}

	void EmbreeSystem::OnUpdateEditor(Timestep ts, EditorCamera& camera)
	{
		(void)ts;
		(void)camera;
		if (mEditorState && mEditorState->bInitialSynchronizationPending)
			(void)SynchronizeEditorPickScene();
	}

	bool EmbreeSystem::SynchronizeEditorPickScene()
	{
		HE_PROFILE;

		if (!mEditorState || !mLevel || !EditorEmbreeContext::IsInitialized())
			return false;

		bool bSceneChanged = false;
		const std::uint64_t contextGeneration = EditorEmbreeContext::GetGeneration();
		if (mEditorState->ContextGeneration != contextGeneration)
		{
			ClearBindings();
			mEditorState->ContextGeneration = contextGeneration;
			bSceneChanged = true;
		}

		auto meshView = mLevel->mRegistry.view<IDComponent, TransformComponent, MeshComponent>();
		std::vector<EntityHandle> snapshot;
		snapshot.reserve(meshView.size_hint());
		for (const EntityHandle handle : meshView)
			snapshot.push_back(handle);

		std::unordered_set<UUID> liveOwners;
		liveOwners.reserve(snapshot.size());
		for (const EntityHandle handle : snapshot)
		{
			// Resource finalization and editor callbacks may mutate the registry.
			// Revalidate every snapshot entry immediately before touching components.
			if (!mLevel->mRegistry.valid(handle) ||
				!mLevel->mRegistry.all_of<IDComponent, TransformComponent, MeshComponent>(handle))
			{
				continue;
			}

			const UUID entityID = mLevel->mRegistry.get<IDComponent>(handle).ID;
			if (!liveOwners.emplace(entityID).second)
			{
				HE_CORE_ERROR("Duplicate entity UUID '{}' cannot be submitted to the editor pick scene.", entityID.str());
				continue;
			}

			Entity entity{ handle, mLevel };
			Ref<Mesh> mesh = mLevel->mRegistry.get<MeshComponent>(handle).mMesh;
			if (!mesh)
			{
				bSceneChanged = bSceneChanged || mEditorState->Bindings.contains(entityID);
				RemoveBinding(entityID);
				continue;
			}

			const glm::mat4 worldTransform = mLevel->GetWorldTransform(entity);
			auto binding = mEditorState->Bindings.find(entityID);
			const bool bNeedsRebuild = binding == mEditorState->Bindings.end() ||
				binding->second.Handle != handle ||
				binding->second.MeshAsset.get() != mesh.get() ||
				!binding->second.Instance ||
				binding->second.Instance->GetContextGeneration() != contextGeneration;

			if (bNeedsRebuild)
			{
				Ref<const MeshEditorPickData> pickData;
				try
				{
					pickData = mesh->GetEditorPickData();
				}
				catch (const std::exception& exception)
				{
					bSceneChanged = bSceneChanged || binding != mEditorState->Bindings.end();
					RemoveBinding(entityID);
					HE_CORE_ERROR("Cannot build editor pick data for entity '{}': {}", entityID.str(), exception.what());
					continue;
				}

				if (!pickData || pickData->Vertices.empty() || pickData->TriangleIndices.empty())
				{
					bSceneChanged = bSceneChanged || binding != mEditorState->Bindings.end();
					RemoveBinding(entityID);
					continue;
				}

				RemoveBinding(entityID);
				try
				{
					Scope<EditorEmbreeInstance> instance = EditorEmbreeContext::CreateInstance(
						pickData->Vertices,
						pickData->TriangleIndices,
						worldTransform);
					const std::uint32_t instanceID = instance->GetInstanceID();

					EditorState::Binding newBinding;
					newBinding.Handle = handle;
					newBinding.MeshAsset = std::move(mesh);
					newBinding.PickData = std::move(pickData);
					newBinding.Instance = std::move(instance);
					newBinding.WorldTransform = worldTransform;

					auto [insertedBinding, bInserted] =
						mEditorState->Bindings.emplace(entityID, std::move(newBinding));
					if (!bInserted)
						throw std::logic_error("The editor Embree entity binding already exists.");

					try
					{
						const auto [owner, bOwnerInserted] =
							mEditorState->InstanceOwners.emplace(instanceID, entityID);
						(void)owner;
						if (!bOwnerInserted)
							throw std::logic_error("The editor Embree instance ID is already owned.");
					}
					catch (...)
					{
						mEditorState->Bindings.erase(insertedBinding);
						throw;
					}
					bSceneChanged = true;
				}
				catch (const std::exception& exception)
				{
					HE_CORE_ERROR("Cannot submit entity '{}' to the editor pick scene: {}", entityID.str(), exception.what());
				}
				continue;
			}

			if (!MatricesEqual(binding->second.WorldTransform, worldTransform))
			{
				if (EditorEmbreeContext::UpdateInstanceTransform(*binding->second.Instance, worldTransform))
				{
					binding->second.WorldTransform = worldTransform;
					bSceneChanged = true;
				}
				else
				{
					RemoveBinding(entityID);
					bSceneChanged = true;
				}
			}
		}

		for (auto binding = mEditorState->Bindings.begin(); binding != mEditorState->Bindings.end();)
		{
			if (liveOwners.contains(binding->first))
			{
				++binding;
				continue;
			}

			if (binding->second.Instance)
				mEditorState->InstanceOwners.erase(binding->second.Instance->GetInstanceID());
			binding = mEditorState->Bindings.erase(binding);
			bSceneChanged = true;
		}

		if (bSceneChanged && !EditorEmbreeContext::Commit())
		{
			HE_CORE_ERROR("Failed to commit the editor Embree pick scene.");
			return false;
		}
		mEditorState->bInitialSynchronizationPending = false;
		return true;
	}

	std::optional<EmbreeEditorPickHit> EmbreeSystem::Raycast(
		const glm::vec3& origin,
		const glm::vec3& direction,
		float nearDistance,
		float farDistance)
	{
		// The author scene may have changed since the last click. Refresh the
		// pick acceleration structure only when a pick actually needs it.
		if (!SynchronizeEditorPickScene())
			return std::nullopt;
		if (!mEditorState || !mLevel || !EditorEmbreeContext::IsInitialized() ||
			mEditorState->ContextGeneration != EditorEmbreeContext::GetGeneration())
		{
			return std::nullopt;
		}

		const std::optional<EditorEmbreeRayHit> hit = EditorEmbreeContext::Raycast(
			origin,
			direction,
			nearDistance,
			farDistance);
		if (!hit)
			return std::nullopt;

		const auto owner = mEditorState->InstanceOwners.find(hit->InstanceID);
		if (owner == mEditorState->InstanceOwners.end())
			return std::nullopt;

		const UUID entityID = owner->second;
		const auto rejectStaleBinding = [this, &entityID]() -> std::optional<EmbreeEditorPickHit>
		{
			RemoveBinding(entityID);
			(void)EditorEmbreeContext::Commit();
			return std::nullopt;
		};

		const auto bindingIt = mEditorState->Bindings.find(entityID);
		if (bindingIt == mEditorState->Bindings.end())
		{
			mEditorState->InstanceOwners.erase(owner);
			return std::nullopt;
		}

		const EditorState::Binding& binding = bindingIt->second;
		if (!binding.Instance || binding.Instance->GetInstanceID() != hit->InstanceID ||
			!mLevel->mRegistry.valid(binding.Handle) ||
			!mLevel->mRegistry.all_of<IDComponent, TransformComponent, MeshComponent>(binding.Handle))
		{
			return rejectStaleBinding();
		}

		try
		{
			if (mLevel->mRegistry.get<IDComponent>(binding.Handle).ID != entityID)
				return rejectStaleBinding();

			const Ref<Mesh>& currentMesh =
				mLevel->mRegistry.get<MeshComponent>(binding.Handle).mMesh;
			if (!currentMesh || currentMesh.get() != binding.MeshAsset.get())
				return rejectStaleBinding();

			const Ref<const MeshEditorPickData> currentPickData = currentMesh->GetEditorPickData();
			if (!currentPickData || currentPickData.get() != binding.PickData.get())
				return rejectStaleBinding();

			Entity entity{ binding.Handle, mLevel };
			if (!MatricesEqual(mLevel->GetWorldTransform(entity), binding.WorldTransform))
				return rejectStaleBinding();

			std::uint32_t subMeshIndex = 0;
			std::uint32_t materialIndex = 0;
			if (!ResolveMeshHitRange(
				binding.PickData->HitRanges,
				hit->PrimitiveID,
				subMeshIndex,
				materialIndex))
			{
				return rejectStaleBinding();
			}

			return EmbreeEditorPickHit{ entityID, subMeshIndex, materialIndex };
		}
		catch (...)
		{
			return rejectStaleBinding();
		}
	}
#endif
}
