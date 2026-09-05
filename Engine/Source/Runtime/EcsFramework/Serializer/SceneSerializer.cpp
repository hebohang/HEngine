#include "Hepch.h"

#include "Runtime/EcsFramework/Serializer/SceneSerializer.h"
#include "Runtime/EcsFramework/Serializer/SceneDocument.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"
#include "Runtime/Core/Meta/Serializer/Serializer.h"
#include "Runtime/Graphics/Mesh/MeshRuntimeResource.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceCatalog.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "all_serializer.h"

#include <chrono>
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace HEngine
{
	SceneSerializer::SceneSerializer(const Ref<Level>& level)
		:mLevel(level)
	{

	}

	static void SerializeEntity(Json& json, Entity entity);

	Json SceneSerializer::SerializeToJson() const
	{
		Json json;

		mLevel->mRegistry.each([&](auto entityID)
			{
				Entity entity = { entityID, mLevel.get() };
				if (!entity)
					return;

				Json entityJson;
				SerializeEntity(entityJson, entity);
				json[entity.UUID()] = entityJson;
			});

		return json;
	}

	template<Component... C>
	static void SerializeEntity(Json& json, Entity entity)
	{
		([&]()
			{
				if (entity.HasComponent<C>())
				{
					auto comp = entity.GetComponent<C>();
					Json compJson;
					Serializer::Serialize(compJson, comp);
					json[C::GetTypeName()] = compJson;
				}
			}(), ...);
	}

	template<Component... C>
	static void SerializeEntity(ComponentGroup<C...>, Json& json, Entity entity)
	{
		SerializeEntity<C...>(json, entity);
	}

	static void SerializeEntity(Json& json, Entity entity)
	{
		HE_CORE_ASSERT(entity.HasComponent<IDComponent>());
		SerializeEntity(ComponentsSerialized{}, json, entity);
	}

	void SceneSerializer::Serialize(const std::string& filepath)
	{
		Utils::SaveJsonToFile(SerializeToJson(), filepath);
	}

	void SceneSerializer::SerializeRuntime(const std::string& filepath)
	{

	}

	template<Component... C>
	static void DeserializeEntity(const Json& valueEntity, Entity& entity)
	{
		([&]()
			{
				if (valueEntity.contains(C::GetTypeName()))
				{
					C comp;
					Serializer::Deserialize(valueEntity[C::GetTypeName()], comp);
					entity.AddComponent(comp);
				}
			}(), ...);
	}

	template<Component... C>
	static void DeserializeEntity(ComponentGroup<C...>, const Json& valueEntity, Entity& entity)
	{
		DeserializeEntity<C...>(valueEntity, entity);
	}

	static void DeserializeEntity(const Json& valueEntity, Entity& entity)
	{
		DeserializeEntity(ComponentsDeserialized{}, valueEntity, entity);
	}

	template<Component... C>
	static void DeserializeStagedEntity(
		ComponentGroup<C...>,
		const Json& valueEntity,
		Entity& entity,
		Registry& registry)
	{
		([&]()
			{
				if (!valueEntity.contains(C::GetTypeName()))
					return;

				C component;
				Serializer::Deserialize(valueEntity[C::GetTypeName()], component);
				if constexpr (std::is_same_v<C, PythonScriptComponent>)
				{
					// Python modules are registered by PythonScriptSystem::OnRuntiemStart.
					// Do not publish script-owned global state from a staging scene.
					registry.emplace<C>(static_cast<EntityHandle>(entity), std::move(component));
				}
				else
				{
					entity.AddComponent(component);
				}
			}(), ...);
	}

	struct IncrementalSceneMaterializer::Impl
	{
		RuntimeResourceRegistry* Registry = nullptr;
		std::shared_ptr<const SceneDocument> Document;
		Ref<Level> StagingLevel;
		Ref<Level> CompletedLevel;
		std::vector<std::pair<std::string, RuntimeResourceKey>> MeshKeys;
		std::exception_ptr Failure;
		SceneMaterializationProgress Progress;

		void RefreshRatio() noexcept
		{
			if (Progress.State == SceneMaterializationState::Ready)
			{
				Progress.Ratio = 1.0f;
				return;
			}

			const size_t totalWork = Progress.Resources.Total + Progress.TotalEntities;
			if (totalWork == 0)
			{
				Progress.Ratio = 0.0f;
				return;
			}

			const size_t completedWork =
				Progress.Resources.Ready + Progress.MaterializedEntities;
			Progress.Ratio = static_cast<float>(completedWork) /
				static_cast<float>(totalWork);
		}

		void ResetOwnedState() noexcept
		{
			CompletedLevel.reset();
			StagingLevel.reset();
			Document.reset();
			MeshKeys.clear();
			Registry = nullptr;
		}

		void StoreCurrentFailure(const std::string& context) noexcept
		{
			const std::exception_ptr cause = std::current_exception();
			try
			{
				if (cause)
				{
					try
					{
						std::rethrow_exception(cause);
					}
					catch (...)
					{
						std::throw_with_nested(std::runtime_error(context));
					}
				}
				throw std::runtime_error(context);
			}
			catch (...)
			{
				Failure = std::current_exception();
			}

			Progress.State = SceneMaterializationState::Failed;
			ResetOwnedState();
			RefreshRatio();
		}
	};

	IncrementalSceneMaterializer::IncrementalSceneMaterializer()
		: mImpl(CreateScope<Impl>())
	{
	}

	IncrementalSceneMaterializer::~IncrementalSceneMaterializer() = default;

	void IncrementalSceneMaterializer::Start(
		RuntimeResourceRegistry& registry,
		std::shared_ptr<const SceneDocument> document)
	{
		Start(
			registry,
			std::move(document),
			ConfigManager::GetInstance().GetRootFolder(),
			RuntimeResourceCatalog::GetInstance().GetSnapshot());
	}

	void IncrementalSceneMaterializer::Start(
		RuntimeResourceRegistry& registry,
		std::shared_ptr<const SceneDocument> document,
		const std::filesystem::path& projectRoot,
		std::shared_ptr<const RuntimeResourceCatalogSnapshot> catalog)
	{
		const SceneMaterializationState previousState = mImpl->Progress.State;
		if (previousState == SceneMaterializationState::WaitingForResources ||
			previousState == SceneMaterializationState::Materializing)
		{
			throw std::logic_error("Cannot restart an active incremental scene materialization.");
		}

		mImpl->ResetOwnedState();
		mImpl->Failure = nullptr;
		mImpl->Progress = {};

		try
		{
			if (!registry.IsInitialized())
				throw std::logic_error("Incremental scene materialization requires an initialized resource registry.");
			if (!document)
				throw std::invalid_argument("Incremental scene materialization requires a scene document.");
			if (!catalog)
				throw std::invalid_argument("Incremental scene materialization requires a catalog snapshot.");

			std::error_code error;
			const std::filesystem::path absoluteRoot = std::filesystem::absolute(projectRoot, error);
			if (error)
				throw std::invalid_argument("Cannot make the scene materialization project root absolute: " + error.message());
			const std::filesystem::path resolvedRoot = std::filesystem::weakly_canonical(absoluteRoot, error);
			if (error || !std::filesystem::is_directory(resolvedRoot, error) || error)
				throw std::invalid_argument("Cannot resolve the scene materialization project root.");

			mImpl->Registry = &registry;
			mImpl->Document = std::move(document);
			mImpl->Progress.TotalEntities = mImpl->Document->GetEntities().size();
			mImpl->StagingLevel = CreateRef<Level>();
			mImpl->StagingLevel->mRuntimeResourceRegistry = &registry;
			mImpl->StagingLevel->mRuntimeResourceProjectRoot = resolvedRoot;
			mImpl->StagingLevel->mRuntimeResourceCatalog = catalog;
			mImpl->StagingLevel->mRuntimeResources = registry.CreateGroup();
			const ConfigManager& config = ConfigManager::GetInstance();
			std::filesystem::path activeProjectRoot;
			if (config.IsInitialized())
			{
				activeProjectRoot = std::filesystem::weakly_canonical(
					config.GetRootFolder(),
					error);
			}
			const bool bUseActiveContentOverlay = config.IsInitialized() &&
				!error && activeProjectRoot == resolvedRoot;

			for (const std::string& meshPath : mImpl->Document->GetMeshPaths())
			{
				std::filesystem::path meshContentRoot = resolvedRoot;
				if (bUseActiveContentOverlay)
				{
					meshContentRoot = ConfigManager::GetContentRootForPath(
						ConfigManager::GetFullPath(meshPath));
				}
				const RuntimeResourceKey key = MeshRuntimeResource::MakeKey(
					meshContentRoot,
					meshPath);
				if (!key.CachePath.ends_with(".hmesh"))
					continue;

				(void)MeshRuntimeResource::QueueAsync(
					registry,
					meshContentRoot,
					catalog,
					meshPath,
					mImpl->StagingLevel->mRuntimeResources);
				mImpl->MeshKeys.emplace_back(meshPath, key);
			}

			mImpl->Progress.Resources = mImpl->StagingLevel->mRuntimeResources.Progress();
			mImpl->Progress.State = SceneMaterializationState::WaitingForResources;
			mImpl->RefreshRatio();
		}
		catch (...)
		{
			mImpl->StoreCurrentFailure("Failed to start incremental scene materialization.");
		}
	}

	SceneMaterializationProgress IncrementalSceneMaterializer::Pump(
		SceneMaterializationBudget budget)
	{
		if (mImpl->Progress.State == SceneMaterializationState::WaitingForResources)
		{
			try
			{
				if (!mImpl->Registry || !mImpl->Document || !mImpl->StagingLevel)
					throw std::logic_error("Incremental scene materialization lost its staging state.");

				mImpl->Progress.Resources = mImpl->StagingLevel->mRuntimeResources.Progress();
				if (mImpl->Progress.Resources.Failed != 0)
				{
					std::string failedPath = "<unknown>";
					for (const auto& [meshPath, key] : mImpl->MeshKeys)
					{
						if (mImpl->Registry->GetState(key) == RuntimeResourceState::Failed)
						{
							try
							{
								mImpl->Registry->RethrowFailure(key);
							}
							catch (...)
							{
								std::throw_with_nested(std::runtime_error(
									"A staged scene mesh failed to load: " + meshPath));
							}
							failedPath = meshPath;
							break;
						}
					}
					throw std::runtime_error("A staged scene mesh failed to load: " + failedPath);
				}
				if (mImpl->Progress.Resources.Unloaded != 0 &&
					mImpl->Progress.Resources.Loading == 0)
				{
					throw std::runtime_error("A staged scene mesh became unavailable before materialization.");
				}

				if (mImpl->Progress.Resources.Ready != mImpl->Progress.Resources.Total)
				{
					mImpl->RefreshRatio();
					return mImpl->Progress;
				}

				mImpl->Progress.State = SceneMaterializationState::Materializing;
			}
			catch (...)
			{
				mImpl->StoreCurrentFailure("Failed while waiting for staged scene resources.");
				return mImpl->Progress;
			}
		}

		if (mImpl->Progress.State != SceneMaterializationState::Materializing ||
			budget.MaxEntities == 0)
		{
			mImpl->RefreshRatio();
			return mImpl->Progress;
		}

		const auto startTime = std::chrono::steady_clock::now();
		const bool bUnlimitedTime = budget.TimeBudget <= std::chrono::nanoseconds::zero();
		size_t materializedThisPump = 0;
		while (mImpl->Progress.MaterializedEntities < mImpl->Progress.TotalEntities &&
			materializedThisPump < budget.MaxEntities)
		{
			if (materializedThisPump != 0 && !bUnlimitedTime &&
				std::chrono::steady_clock::now() - startTime >= budget.TimeBudget)
			{
				break;
			}

			const SceneDocumentEntity& documentEntity =
				mImpl->Document->GetEntities()[mImpl->Progress.MaterializedEntities];
			try
			{
#if !WITH_EDITOR
				if (documentEntity.Value.contains("SceneFolderComponent"))
				{
					++mImpl->Progress.MaterializedEntities;
					++materializedThisPump;
					continue;
				}
#endif
				Entity entity = mImpl->StagingLevel->CreateEntityWithoutTrans(
					documentEntity.Uuid,
					documentEntity.Tag);
				DeserializeStagedEntity(
					ComponentsDeserialized{},
					documentEntity.Value,
					entity,
					mImpl->StagingLevel->mRegistry);
			}
			catch (...)
			{
				mImpl->StoreCurrentFailure(
					"Failed to materialize scene entity '" + documentEntity.Uuid + "'.");
				return mImpl->Progress;
			}

			++mImpl->Progress.MaterializedEntities;
			++materializedThisPump;
		}

		if (mImpl->Progress.MaterializedEntities == mImpl->Progress.TotalEntities)
		{
			mImpl->CompletedLevel = std::move(mImpl->StagingLevel);
			mImpl->Document.reset();
			mImpl->MeshKeys.clear();
			mImpl->Registry = nullptr;
			mImpl->Progress.State = SceneMaterializationState::Ready;
		}

		mImpl->RefreshRatio();
		return mImpl->Progress;
	}

	SceneMaterializationProgress IncrementalSceneMaterializer::GetProgress() const noexcept
	{
		return mImpl->Progress;
	}

	void IncrementalSceneMaterializer::RequestCancel()
	{
		if (mImpl->Progress.State != SceneMaterializationState::WaitingForResources &&
			mImpl->Progress.State != SceneMaterializationState::Materializing)
		{
			return;
		}

		mImpl->Failure = std::make_exception_ptr(std::runtime_error(
			"Incremental scene materialization was cancelled after " +
			std::to_string(mImpl->Progress.MaterializedEntities) + " of " +
			std::to_string(mImpl->Progress.TotalEntities) + " entities."));
		mImpl->Progress.State = SceneMaterializationState::Cancelled;
		mImpl->ResetOwnedState();
		mImpl->RefreshRatio();
	}

	void IncrementalSceneMaterializer::RethrowFailure() const
	{
		if (mImpl->Failure)
			std::rethrow_exception(mImpl->Failure);
	}

	Ref<Level> IncrementalSceneMaterializer::TakeCompletedLevel()
	{
		if (mImpl->Progress.State != SceneMaterializationState::Ready)
			return nullptr;
		return std::exchange(mImpl->CompletedLevel, nullptr);
	}

	bool SceneSerializer::DeserializeFromJson(const Json& json)
	{
		for (auto& [keyEntity, valueEntity] : json.items())
		{
			if (!valueEntity.contains("TagComponent") || !valueEntity["TagComponent"].contains("Tag"))
				continue;
#if !WITH_EDITOR
			if (valueEntity.contains("SceneFolderComponent"))
				continue;
#endif

			Entity deserializedEntity = mLevel->CreateEntityWithoutTrans(keyEntity, valueEntity["TagComponent"]["Tag"].get<std::string>());
			DeserializeEntity(valueEntity, deserializedEntity);
		}

		return true;
	}

	bool SceneSerializer::Deserialize(const std::string& filepath)
	{
		HE_PROFILE_SECTION("SceneSerializer::Deserialize");
		return DeserializeFromJson(Utils::LoadJsonFile(filepath));
	}

	bool SceneSerializer::DeserializeRuntime(const std::string& filepath)
	{
		//have not implemented
		HE_CORE_ASSERT(false);
		return false;
	}
}
