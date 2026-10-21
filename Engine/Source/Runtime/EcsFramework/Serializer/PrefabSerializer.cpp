#include "Hepch.h"

#include "Runtime/EcsFramework/Serializer/PrefabSerializer.h"
#include "Runtime/EcsFramework/Serializer/PrefabDocument.h"
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
#include <set>

namespace HEngine
{
    Entity PrefabSerializer::FindInstanceEntity(Entity member, const std::string& localId)
    {
        if (!member || !member.HasComponent<PrefabInstanceComponent>()) return {};
        Level* level = member.GetLevel();
        const auto root = member.GetComponent<PrefabInstanceComponent>().InstanceRootUuid;
        for (auto handle : level->GetAllEntitiesWith<PrefabInstanceComponent>())
        {
            Entity candidate(handle, level);
            const auto& identity = candidate.GetComponent<PrefabInstanceComponent>();
            if (identity.InstanceRootUuid == root && identity.LocalId == localId) return candidate;
        }
        return {};
    }
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
        Json scene = Json::object();
        Level* level = entity.GetLevel();
        for (auto handle : level->GetAllEntitiesWith<IDComponent>())
        {
            Entity item(handle, level);
            Json value = Json::object();
            SerializePrefabEntity(ComponentsSerialized{}, value, item);
            scene[item.GetComponent<IDComponent>().ID.str()] = std::move(value);
        }
        const Json json = CapturePrefabDocument(scene, entity.GetComponent<IDComponent>().ID.str(), path.Key.Path);
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

    template<Component... C>
    static Json CanonicalPrefabEntity(ComponentGroup<C...>, const Json& value)
    {
        Json result = Json::object();
        const std::set<std::string> names{ C::GetTypeName()... };
        for (auto& [name, fields] : value.items())
            if ((!names.contains(name) && name != "EditorEntityStateComponent" && name != "SceneFolderComponent") || !fields.is_object())
                throw std::invalid_argument("Unknown or malformed prefab component: " + name);
        ([&] {
            if (!value.contains(C::GetTypeName())) return;
            C component; Json defaults; Serializer::Serialize(defaults, component);
            for (auto& [field, unused] : value.at(C::GetTypeName()).items())
                if (!defaults.contains(field)) throw std::invalid_argument("Unknown prefab field: " + std::string(C::GetTypeName()) + "." + field);
            Serializer::Deserialize(value.at(C::GetTypeName()), component);
            Serializer::Serialize(result[C::GetTypeName()], component);
        }(), ...);
        if (result.contains("FollowCameraComponent") && !result.at("FollowCameraComponent").value("bHasTarget",false))
            result["FollowCameraComponent"]["TargetUUID"] = "00000000-0000-0000-0000-000000000000";
        return result;
    }

    Entity PrefabSerializer::InstantiateFromJson(Level* level, const Json& prefabJson, const std::string& path)
    {
        auto loader = [&](const std::string& dependency) {
            const auto resolved = ResolveRuntimePrefabPath(ConfigManager::GetInstance().GetRootFolder(), dependency);
            auto& registry = RuntimeResourceRegistry::GetInstance();
            if (!registry.IsInitialized()) return Utils::LoadJsonFile(resolved.FullPath);
            if (!level->mRuntimeResources) level->mRuntimeResources = registry.CreateGroup();
            auto handle = AcquireRuntimePrefabJson(registry, resolved);
            if (!level->mRuntimeResources.Pin(handle)) throw std::runtime_error("Failed to pin nested prefab: " + dependency);
            return handle->GetDocument();
        };
        auto expanded = ExpandPrefabDocument(prefabJson, loader, path);
        for (auto& [id, value] : expanded.Entities.items()) value = CanonicalPrefabEntity(ComponentsSerialized{}, value);
        return InstantiatePrepared(level, expanded, path);
    }

    Entity PrefabSerializer::InstantiatePrepared(Level* level, const PrefabDocument& prefab, const std::string& path)
    {
        const auto prepared = InstantiatePrefabDocument(prefab, path, [] { return UUID().str(); });
        std::vector<Entity> created;
        try
        {
            // Allocate every identity before components resolve internal refs.
            for (auto& [id, value] : prepared.Values.items())
                created.push_back(level->CreateEntityWithoutTrans(UUID(id), value.at("TagComponent").at("Tag")));
            for (Entity entity : created)
                DeserializePrefabEntity(ComponentsDeserialized{}, prepared.Values.at(entity.GetComponent<IDComponent>().ID.str()), entity);
            // Scripts observe a fully assembled subtree, never a partial graph.
            if (PythonScriptSystem* scripts = PythonScriptSystem::GetActiveSystem(level))
                for (Entity entity : created)
                    if (entity.HasComponent<PythonScriptComponent>())
                    {
                        const auto& scriptPath = entity.GetComponent<PythonScriptComponent>().Path;
                        if (!scriptPath.empty() && scriptPath != "None" && !scripts->StartRuntimeOwner(entity))
                            throw std::runtime_error("Prefab script initialization failed");
                    }
            for (Entity entity : created)
                if (!entity) throw std::runtime_error("Prefab entity was destroyed while initializing scripts");
        }
        catch (...)
        {
            const std::exception_ptr failure = std::current_exception();
            try
            {
                for (Entity entity : created)
                    if (entity && entity.HasComponent<TransformComponent>()) entity.GetComponent<TransformComponent>().ParentUuid.clear();
                for (Entity entity : created) if (entity) level->DestroyEntity(entity);
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

        return level->TryGetEntityByUUID(UUID(prepared.Root));
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
            return InstantiateFromJson(level, Utils::LoadJsonFile(path.FullPath), path.Key.Path);

        if (!level->mRuntimeResources)
            level->mRuntimeResources = registry.CreateGroup();
        RuntimeResourceHandle<RuntimePrefabJson> handle = AcquireRuntimePrefabJson(registry, path);
        if (!level->mRuntimeResources.Pin(handle))
            throw std::logic_error("Failed to pin prefab runtime resource: " + path.Key.Path);
		const Json& document = handle->GetDocument();
		// A nested prefab can change independently of its parent generation.
		// Keep its dependency expansion on the uncached path.
		if (!document.contains("schemaVersion") ||
			(document.contains("nested") && document.at("nested").is_array() && document.at("nested").empty()))
		{
			auto& cached = level->mPreparedPrefabs[path.FullPath.generic_string()];
			if (!cached.second || cached.first != handle.GetGeneration())
			{
				auto expanded = ExpandPrefabDocument(document, {}, path.Key.Path);
				for (auto& [id, value] : expanded.Entities.items())
					value = CanonicalPrefabEntity(ComponentsSerialized{}, value);
				cached = {handle.GetGeneration(), std::make_shared<const PrefabDocument>(std::move(expanded))};
			}
			return InstantiatePrepared(level, *cached.second, path.Key.Path);
		}
        return InstantiateFromJson(level, handle->GetDocument(), path.Key.Path);
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
