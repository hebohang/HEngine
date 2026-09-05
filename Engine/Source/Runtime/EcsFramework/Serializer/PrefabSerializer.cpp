#include "Hepch.h"

#include "Runtime/EcsFramework/Serializer/PrefabSerializer.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/System/Script/PythonScriptSystem.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/Core/Meta/Serializer/Serializer.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimePrefabResource.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "all_serializer.h"

#include <exception>
#include <filesystem>
#include <stdexcept>

namespace HEngine
{
    template<Component... C>
    static void SerializePrefabEntity(Json& json, Entity entity)
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
    static void SerializePrefabEntity(ComponentGroup<C...>, Json& json, Entity entity)
    {
        SerializePrefabEntity<C...>(json, entity);
    }

    void PrefabSerializer::SavePrefab(Entity entity, const std::string& filepath)
    {
        HE_CORE_ASSERT(entity.HasComponent<IDComponent>());

        RuntimePrefabPath path;
        try
        {
            path = ResolveRuntimePrefabWrittenPath(
                ConfigManager::GetInstance().GetRootFolder(),
                filepath);
        }
        catch (const std::invalid_argument& ex)
        {
            HE_CORE_ERROR("[PrefabSerializer] Invalid prefab save path '{0}': {1}", filepath, ex.what());
            return;
        }
        Json json;
        SerializePrefabEntity(ComponentsSerialized{}, json, entity);
        Utils::SaveJsonToFile(json, path.FullPath);

        RuntimeResourceRegistry& registry = RuntimeResourceRegistry::GetInstance();
        if (registry.IsInitialized())
            (void)registry.Invalidate(path.Key);
        HE_CORE_INFO("[PrefabSerializer] Saved prefab to: {0}", path.FullPath.string());
    }

    template<Component... C>
    static void DeserializePrefabEntity(const Json& prefabJson, Entity& entity)
    {
        ([&]()
            {
                if (prefabJson.contains(C::GetTypeName()))
                {
                    C comp;
                    Serializer::Deserialize(prefabJson[C::GetTypeName()], comp);
                    entity.AddComponent(comp);
                }
            }(), ...);
    }

    template<Component... C>
    static void DeserializePrefabEntity(ComponentGroup<C...>, const Json& prefabJson, Entity& entity)
    {
        DeserializePrefabEntity<C...>(prefabJson, entity);
    }

    Entity PrefabSerializer::InstantiateFromJson(Level* level, const Json& prefabJson)
    {
        std::string tagName = "Entity";
        if (prefabJson.contains("TagComponent") && prefabJson["TagComponent"].contains("Tag"))
        {
            tagName = prefabJson["TagComponent"]["Tag"].get<std::string>();
        }

        Entity entity = level->CreateEntityWithoutTrans(UUID(), tagName);
        try
        {
            DeserializePrefabEntity(ComponentsDeserialized{}, prefabJson, entity);

			if (entity.HasComponent<PythonScriptComponent>())
			{
				if (PythonScriptSystem* scriptSystem =
					PythonScriptSystem::GetActiveSystem(level))
				{
					(void)scriptSystem->StartRuntimeOwner(entity);
				}
			}
        }
        catch (...)
        {
            const std::exception_ptr failure = std::current_exception();
            try
            {
                if (entity)
                    level->DestroyEntity(entity);
            }
            catch (const std::exception& cleanupFailure)
            {
                Log::WriteFallbackError(
                    "Failed to roll back a partially instantiated prefab entity.",
                    cleanupFailure.what());
            }
            catch (...)
            {
                Log::WriteFallbackError(
                    "Failed to roll back a partially instantiated prefab entity.",
                    "unknown exception");
            }
            std::rethrow_exception(failure);
        }

        return entity;
    }

    Entity PrefabSerializer::InstantiatePrefab(Level* level, const std::string& prefabPath)
    {
        if (!level)
        {
            HE_CORE_ERROR("[PrefabSerializer] InstantiatePrefab called with null level");
            return {};
        }

        RuntimePrefabPath path;
        try
        {
            path = ResolveRuntimePrefabPath(
                ConfigManager::GetInstance().GetRootFolder(),
                prefabPath);
        }
        catch (const std::invalid_argument& ex)
        {
            HE_CORE_ERROR("[PrefabSerializer] Invalid prefab path '{0}': {1}", prefabPath, ex.what());
            return {};
        }

        if (!std::filesystem::exists(path.FullPath))
        {
            HE_CORE_ERROR("[PrefabSerializer] Prefab file not found: {0}", path.FullPath.string());
            return {};
        }

        RuntimeResourceRegistry& registry = RuntimeResourceRegistry::GetInstance();
        if (!registry.IsInitialized())
            return InstantiateFromJson(level, Utils::LoadJsonFile(path.FullPath));

        if (!level->mRuntimeResources)
            level->mRuntimeResources = registry.CreateGroup();
        RuntimeResourceHandle<RuntimePrefabJson> handle = AcquireRuntimePrefabJson(registry, path);
        if (!level->mRuntimeResources.Pin(handle))
            throw std::logic_error("Failed to pin prefab runtime resource: " + path.Key.Path);
        return InstantiateFromJson(level, handle->GetDocument());
    }

    Entity PrefabSerializer::InstantiatePrefab(Level* level, const std::string& prefabPath, const glm::vec3& position)
    {
        Entity entity = InstantiatePrefab(level, prefabPath);
        if (entity && entity.HasComponent<TransformComponent>())
        {
            entity.GetComponent<TransformComponent>().Translation = position;
        }
        return entity;
    }

    Entity PrefabSerializer::InstantiatePrefab(Level* level, const std::string& prefabPath, const glm::vec3& position, const glm::vec3& rotation)
    {
        Entity entity = InstantiatePrefab(level, prefabPath);
        if (entity && entity.HasComponent<TransformComponent>())
        {
            auto& transform = entity.GetComponent<TransformComponent>();
            transform.Translation = position;
            transform.Rotation = rotation;
        }
        return entity;
    }
}
