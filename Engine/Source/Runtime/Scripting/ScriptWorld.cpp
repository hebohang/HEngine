#include "ScriptWorld.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/EcsFramework/Component/Basic/IDComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Component/Shape/SpriteRendererComponent.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimePrefabResource.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <cstdlib>
#include <string_view>
#include <limits>
#include <stdexcept>
#include <utility>

namespace HEngine
{
    namespace
    {
        struct HandleEntry { ScriptWorld* World; EntityHandle Entity; };
        std::map<ScriptEntityHandle, HandleEntry> sHandles;
        ScriptEntityHandle sNextHandle = 1;
        std::uint64_t sNextComponentGeneration = 1;
        std::uint64_t sNextResourceRequest = 1;
        std::uint64_t sNextServiceLease = 1;
    }

    struct ScriptWorld::ResourceRequest
    {
        ScriptEntityHandle Owner;
        std::string Path;
        RuntimeResourceGroup Group;
    };

    struct ScriptWorld::ServiceLease
    {
        ScriptEntityHandle Owner;
        std::string Kind;
        std::uint64_t Value;
        std::function<void()> Cleanup;
        ~ServiceLease() { try { if (Cleanup) Cleanup(); } catch (...) {} }
    };

    std::uint64_t ScriptWorld::OwnService(ScriptEntityHandle owner, std::string kind, std::uint64_t value, std::function<void()> cleanup)
    {
        // Own the callback before validating; rejection must also release the
        // just-created native playback/document.
        auto lease = CreateScope<ServiceLease>();
        lease->Owner = owner; lease->Kind = std::move(kind); lease->Value = value; lease->Cleanup = std::move(cleanup);
        if (!Resolve(owner)) throw std::invalid_argument("Service owner is no longer alive.");
        auto& world = *sHandles.at(owner).World;
        if (world.mServiceLeases.size() >= 4096 || sNextServiceLease >= (std::uint64_t{1} << 53) - 1)
            throw std::runtime_error("Scene service lease budget exhausted.");
        const auto id = sNextServiceLease++;
        world.mServiceLeases.emplace(id, std::move(lease));
        return id;
    }

    std::uint64_t ScriptWorld::ReadService(ScriptEntityHandle owner, const std::string& kind, std::uint64_t lease)
    {
        if (!Resolve(owner)) throw std::invalid_argument("Service owner is no longer alive.");
        auto& leases = sHandles.at(owner).World->mServiceLeases;
        const auto found = leases.find(lease);
        if (found == leases.end() || found->second->Owner != owner || found->second->Kind != kind)
            throw std::invalid_argument("Service lease is disposed, belongs to another owner or has the wrong type.");
        return found->second->Value;
    }

    bool ScriptWorld::ReleaseService(ScriptEntityHandle owner, const std::string& kind, std::uint64_t lease)
    {
        const auto context = sHandles.find(owner);
        if (context == sHandles.end()) return false;
        auto& leases = context->second.World->mServiceLeases;
        const auto found = leases.find(lease);
        if (found == leases.end()) return false;
        if (found->second->Owner != owner || found->second->Kind != kind)
            throw std::invalid_argument("Service lease belongs to another owner or type.");
        leases.erase(found); return true;
    }

    void ScriptWorld::ReleaseServices(ScriptEntityHandle owner)
    {
        const auto context = sHandles.find(owner);
        if (context == sHandles.end()) return;
        std::erase_if(context->second.World->mServiceLeases, [owner](const auto& pair) { return pair.second->Owner == owner; });
    }

    void ScriptWorld::ReleaseEntityServices(EntityHandle entity)
    {
        const auto found = mHandles.find(entity);
        if (found != mHandles.end()) ReleaseServices(found->second);
    }

    std::uint64_t ScriptWorld::PrefabRequest(ScriptEntityHandle owner, std::uint64_t request, std::uint32_t operation, const std::string& path)
    {
#if defined(__EMSCRIPTEN__)
        throw std::invalid_argument("Prefab async loading requires the desktop resource registry.");
#else
        const auto context = sHandles.find(owner);
        if (context == sHandles.end())
        {
            if (operation == 2) return 0; // Scene destruction already released this owner's leases.
            throw std::invalid_argument("Prefab request owner is no longer alive.");
        }
        auto& world = *context->second.World;
        if (operation == 2)
        {
            const auto found = world.mResourceRequests.find(request);
            if (found != world.mResourceRequests.end() && found->second->Owner != owner)
                throw std::invalid_argument("Prefab request belongs to another script owner.");
            world.mResourceRequests.erase(request);
            return 0;
        }
        if (!Resolve(owner)) throw std::invalid_argument("Prefab request owner is pending destruction.");
        auto* registry = world.mLevel->GetRuntimeResourceRegistry();
        if (!registry) registry = &RuntimeResourceRegistry::GetInstance();
        if (!registry->IsInitialized()) throw std::logic_error("This scene has no initialized asynchronous resource registry.");
        if (operation == 0)
        {
            if (world.mResourceRequests.size() >= 1024 || sNextResourceRequest >= (std::uint64_t{1} << 53))
                throw std::overflow_error("Prefab request limit reached.");
            const auto resolved = ResolveRuntimePrefabPath(ConfigManager::GetProjectFullPath(""), path);
            auto pending = CreateScope<ResourceRequest>();
            pending->Owner = owner; pending->Path = path; pending->Group = registry->CreateGroup();
            (void)QueueRuntimePrefabJson(*registry, resolved, pending->Group);
            const auto id = sNextResourceRequest++;
            world.mResourceRequests.emplace(id, std::move(pending));
            return id;
        }
        const auto found = world.mResourceRequests.find(request);
        if (found == world.mResourceRequests.end() || found->second->Owner != owner)
            throw std::invalid_argument("Prefab request is released or belongs to another script owner.");
        const auto progress = found->second->Group.Progress();
        if (progress.Failed) throw std::runtime_error("Prefab asynchronous load failed: " + found->second->Path);
        if (operation == 1) return progress.Total == 1 && progress.Ready == 1 ? 1 : 0;
        if (operation == 3)
        {
            if (progress.Total != 1 || progress.Ready != 1) throw std::logic_error("Prefab is not ready for instantiation.");
            return world.Bind(world.mLevel->InstantiatePrefab(found->second->Path));
        }
        throw std::invalid_argument("Invalid prefab request operation.");
#endif
    }

    ScriptWorld::ScriptWorld(Level& level) : mLevel(&level),
        mDestroyConnection(level.mRegistry.on_destroy<IDComponent>().connect<&ScriptWorld::OnEntityDestroyed>(*this)),
        mTransformConnection(level.mRegistry.on_destroy<TransformComponent>().connect<&ScriptWorld::OnTransformDestroyed>(*this)),
        mSpriteConnection(level.mRegistry.on_destroy<SpriteRendererComponent>().connect<&ScriptWorld::OnSpriteDestroyed>(*this)) {}

    ScriptWorld::~ScriptWorld() { Close(); }

    ScriptEntityHandle ScriptWorld::Bind(Entity entity)
    {
        if (!mLevel || !entity || entity.GetLevel() != mLevel || mPendingDestroy.contains(entity)) return 0;
        const EntityHandle raw = entity;
        if (const auto it = mHandles.find(raw); it != mHandles.end()) return it->second;
#if defined(__EMSCRIPTEN__)
        constexpr ScriptEntityHandle limit = (ScriptEntityHandle{1} << 53) - 1;
#else
        constexpr ScriptEntityHandle limit = std::numeric_limits<ScriptEntityHandle>::max();
#endif
        if (sNextHandle >= limit)
            throw std::overflow_error("Script entity handle space exhausted.");
        const auto id = sNextHandle++;
        sHandles.emplace(id, HandleEntry{this, raw});
        mHandles.emplace(raw, id);
        return id;
    }

    Entity ScriptWorld::Resolve(ScriptEntityHandle handle)
    {
        const auto it = sHandles.find(handle);
        if (it == sHandles.end()) return {};
        auto& world = *it->second.World;
        if (!world.mLevel || world.mPendingDestroy.contains(it->second.Entity)) return {};
        Entity entity{it->second.Entity, world.mLevel};
        return entity ? entity : Entity{};
    }

    bool ScriptWorld::RequestDestroy(ScriptEntityHandle handle)
    {
        if (!Resolve(handle)) return false;
        const auto entry = sHandles.at(handle);
        entry.World->mPendingDestroy.insert(entry.Entity);
        return true;
    }

    ScriptEntityHandle ScriptWorld::BindInContext(ScriptEntityHandle context, Entity entity)
    {
        if (!Resolve(context)) throw std::invalid_argument("A live script context is required.");
        return sHandles.at(context).World->Bind(entity);
    }

    ScriptEntityHandle ScriptWorld::Find(ScriptEntityHandle context, const std::string& stableId)
    {
        if (!Resolve(context) || stableId.empty()) return 0;
        auto* world = sHandles.at(context).World;
        return world->Bind(world->mLevel->TryGetEntityByUUID(UUID(stableId)));
    }

    void ScriptWorld::Retire(EntityHandle entity)
    {
        if (const auto it = mHandles.find(entity); it != mHandles.end())
        {
            std::erase_if(mResourceRequests, [&](const auto& entry) { return entry.second->Owner == it->second; });
            ReleaseServices(it->second);
            sHandles.erase(it->second);
            mHandles.erase(it);
        }
        mPendingDestroy.erase(entity);
    }

    void ScriptWorld::EmitEvent(ScriptEntityHandle context, const std::string& name, std::string payload)
    {
        if (!Resolve(context)) throw std::invalid_argument("Script events require a live scene entity context.");
        auto& world = *sHandles.at(context).World;
        if (name.empty() || name.size() > 128 || payload.size() > 65536 ||
            (!world.mEvents.contains(name) && world.mEvents.size() >= 1024))
            throw std::invalid_argument("Script event name/payload limit exceeded.");
        // Sequence values travel through JS numbers on portable hosts.
        if (world.mEventSequence >= (std::uint64_t{1} << 53) - 1) throw std::overflow_error("Script event sequence exhausted.");
        world.mEvents[name] = {++world.mEventSequence, std::move(payload)};
    }

    ScriptWorld::Event ScriptWorld::ReadEvent(ScriptEntityHandle context, const std::string& name, std::uint64_t after)
    {
        if (!Resolve(context)) throw std::invalid_argument("Script events require a live scene entity context.");
        const auto& events = sHandles.at(context).World->mEvents;
        if (const auto it = events.find(name); it != events.end() && it->second.Sequence > after)
        {
            static const bool traceInput = [] { const char* value = std::getenv("HE_TRACE_UI_INPUT"); return value && std::string_view(value) == "1"; }();
            if (traceInput && name == "ui.event")
                HE_CORE_INFO("[UIInput] mailbox read context={} after={} sequence={} payload={}",
                    context, after, it->second.Sequence, it->second.Payload);
            return it->second;
        }
        return {after, {}};
    }

    void ScriptWorld::OnEntityDestroyed(Registry&, EntityHandle entity) { Retire(entity); }
    void ScriptWorld::OnTransformDestroyed(Registry&, EntityHandle entity) { mComponentGenerations.erase({entity, 1}); }
    void ScriptWorld::OnSpriteDestroyed(Registry&, EntityHandle entity) { mComponentGenerations.erase({entity, 2}); }

    std::uint64_t ScriptWorld::ComponentGeneration(ScriptEntityHandle context, std::uint32_t kind)
    {
        auto entity = Resolve(context);
        if (!entity) throw std::invalid_argument("Component query requires a live entity.");
        if (kind != 1 && kind != 2) throw std::invalid_argument("Unsupported component binding ID.");
        if ((kind == 1 && !entity.HasComponent<TransformComponent>()) || (kind == 2 && !entity.HasComponent<SpriteRendererComponent>())) return 0;
        auto& generations = sHandles.at(context).World->mComponentGenerations;
        const auto key = std::pair{static_cast<EntityHandle>(entity), kind};
        if (const auto found = generations.find(key); found != generations.end()) return found->second;
        if (sNextComponentGeneration >= (std::uint64_t{1} << 53) - 1) throw std::overflow_error("Component generation limit reached.");
        const auto generation = sNextComponentGeneration++;
        generations.emplace(key, generation);
        return generation;
    }

    void ScriptWorld::FlushCommands()
    {
        // Destruction may enqueue more commands. Those belong to the next barrier.
        const auto pending = mPendingDestroy;
        for (const auto raw : pending)
        {
            if (!mLevel) break;
            if (Entity entity{raw, mLevel}; entity) mLevel->DestroyEntity(entity);
            Retire(raw);
        }
    }

    void ScriptWorld::Close()
    {
        mServiceLeases.clear();
        mResourceRequests.clear();
        mDestroyConnection.release();
        mTransformConnection.release();
        mSpriteConnection.release();
        mComponentGenerations.clear();
        for (const auto& [raw, id] : mHandles) sHandles.erase(id);
        mHandles.clear();
        mPendingDestroy.clear();
        mEvents.clear();
        mEventSequence = 0;
        mLevel = nullptr;
    }
}
