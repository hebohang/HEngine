#pragma once

#include "Runtime/EcsFramework/System/System.h"
#include "Runtime/Scripting/ScriptCore.h"
#include "Runtime/Scripting/ScriptWorld.h"

#include <map>
#include <set>

namespace HEngine
{
    class ScriptSystem final : public System
    {
    public:
        explicit ScriptSystem(Level* level);
        ~ScriptSystem() override;
        void OnRuntiemStart() override;
        void OnRuntimeStop() override;
        void OnUpdateRuntime(Timestep ts) override;
        void SetPaused(bool paused) { if (mCore) mCore->SetPaused(paused); }
        void NotifyApplicationQuit() { if (mCore) mCore->Notify(ScriptCallback::ApplicationQuit); }
        void QueueContact(EntityHandle receiver, EntityHandle other, std::uint32_t phase, bool trigger,
            const glm::vec3& point, const glm::vec3& normal);

    private:
        struct Owner
        {
            ScriptInstanceHandle Instance = 0;
            std::string Backend;
            std::string Type;
            bool FieldFault = false;
            std::string LastFields;
        };
        void Synchronize();
        void OnComponentRemoving(Registry&, EntityHandle entity);
        std::shared_ptr<ScriptWorld> mWorld;
        std::shared_ptr<ScriptCore> mCore;
        std::map<EntityHandle, Owner> mOwners;
        std::set<std::string> mRegistered;
        entt::scoped_connection mComponentConnection;
        struct PendingContact { EntityHandle Receiver; ScriptContactEvent Event; };
        std::vector<PendingContact> mContacts;
    };
}
