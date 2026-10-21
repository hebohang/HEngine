#pragma once

#include "ScriptCore.h"
#include "Runtime/EcsFramework/Entity/Entity.h"

#include <map>
#include <set>

namespace HEngine
{
    // A scene-owned table gives scripts monotonically allocated opaque handles.
    // Destroy notifications retire them permanently, including entt ID reuse.
    class ScriptWorld final
    {
    public:
        explicit ScriptWorld(Level& level);
        ~ScriptWorld();
        ScriptWorld(const ScriptWorld&) = delete;
        ScriptWorld& operator=(const ScriptWorld&) = delete;
        ScriptEntityHandle Bind(Entity entity);
        static Entity Resolve(ScriptEntityHandle handle);
        static ScriptEntityHandle BindInContext(ScriptEntityHandle context, Entity entity);
        static ScriptEntityHandle Find(ScriptEntityHandle context, const std::string& stableId);
        static std::uint64_t ComponentGeneration(ScriptEntityHandle context, std::uint32_t kind);
        static bool RequestDestroy(ScriptEntityHandle handle);
        static std::uint64_t PrefabRequest(ScriptEntityHandle owner, std::uint64_t request, std::uint32_t operation, const std::string& path);
        struct Event { std::uint64_t Sequence = 0; std::string Payload; };
        static void EmitEvent(ScriptEntityHandle context, const std::string& name, std::string payload);
        static Event ReadEvent(ScriptEntityHandle context, const std::string& name, std::uint64_t after);
        static std::uint64_t OwnService(ScriptEntityHandle owner, std::string kind, std::uint64_t value, std::function<void()> cleanup);
        static std::uint64_t ReadService(ScriptEntityHandle owner, const std::string& kind, std::uint64_t lease);
        static bool ReleaseService(ScriptEntityHandle owner, const std::string& kind, std::uint64_t lease);
        static void ReleaseServices(ScriptEntityHandle owner);
        void ReleaseEntityServices(EntityHandle entity);
        void FlushCommands();
        void Close();

    private:
        void OnEntityDestroyed(Registry&, EntityHandle entity);
        void OnTransformDestroyed(Registry&, EntityHandle entity);
        void OnSpriteDestroyed(Registry&, EntityHandle entity);
        void Retire(EntityHandle entity);
        Level* mLevel;
        struct ResourceRequest;
        std::map<std::uint64_t, Scope<ResourceRequest>> mResourceRequests;
        struct ServiceLease;
        std::map<std::uint64_t, Scope<ServiceLease>> mServiceLeases;
        std::map<EntityHandle, ScriptEntityHandle> mHandles;
        std::set<EntityHandle> mPendingDestroy;
        std::map<std::string, Event> mEvents;
        std::uint64_t mEventSequence = 0;
        entt::scoped_connection mDestroyConnection;
        entt::scoped_connection mTransformConnection;
        entt::scoped_connection mSpriteConnection;
        std::map<std::pair<EntityHandle, std::uint32_t>, std::uint64_t> mComponentGenerations;
    };
}
