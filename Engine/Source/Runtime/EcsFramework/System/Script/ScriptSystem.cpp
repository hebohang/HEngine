#include "ScriptSystem.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/EcsFramework/Component/Script/ScriptComponent.h"
#include "Runtime/EcsFramework/Component/Basic/IDComponent.h"
#include "Runtime/Core/SimulationClock.h"
#include "Runtime/Scripting/ScriptBackendRegistry.h"
#include "Runtime/Scripting/ScriptFieldsJson.h"
#include "Runtime/Scripting/ScriptEngineApi.h"
#include "Runtime/Profile/Profile.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <set>
#include <tuple>

namespace HEngine
{
    ScriptSystem::ScriptSystem(Level* level) : System(level),
        mComponentConnection(level->mRegistry.on_destroy<ScriptComponent>().connect<&ScriptSystem::OnComponentRemoving>(*this)) {}

    ScriptSystem::~ScriptSystem() { OnRuntimeStop(); }

    void ScriptSystem::OnRuntiemStart()
    {
        OnRuntimeStop();
        ScriptEngineApi::BindThread();
        mWorld = std::make_shared<ScriptWorld>(*mLevel);
#if defined(HE_WEB)
        constexpr auto platform = ScriptPlatform::Web;
#elif defined(HE_WECHAT)
        constexpr auto platform = ScriptPlatform::WeChat;
#elif defined(__EMSCRIPTEN__)
        constexpr auto platform = ScriptPlatform::Web;
#elif defined(__ANDROID__)
        constexpr auto platform = ScriptPlatform::Android;
#else
        constexpr auto platform = ScriptPlatform::Windows;
#endif
        mCore = std::make_shared<ScriptCore>(platform,
            [](ScriptEntityHandle id) { return bool(ScriptWorld::Resolve(id)); },
            [](ScriptInstanceHandle id, const ScriptError& error) {
                HE_CORE_ERROR("[Script] instance={0}, kind={1}: {2}\n{3}", id, static_cast<int>(error.Kind), error.Message, error.Stack);
            });
        Synchronize();
        if (mCore) mCore->Notify(ScriptCallback::SceneLoaded);
    }

    void ScriptSystem::OnComponentRemoving(Registry&, EntityHandle entity)
    {
        const auto it = mOwners.find(entity);
        if (it == mOwners.end()) return;
        const auto instance = it->second.Instance;
        const auto world = mWorld;
        mOwners.erase(it);
        if (mCore && instance) mCore->Remove(instance);
        if (world) world->ReleaseEntityServices(entity);
    }

    void ScriptSystem::OnRuntimeStop()
    {
        mContacts.clear();
        if (mCore) mCore->Stop();
        mCore.reset();
        mOwners.clear();
        mRegistered.clear();
        if (mWorld) mWorld->Close();
        mWorld.reset();
    }

    void ScriptSystem::Synchronize()
    {
        HE_PROFILE_SECTION("Script.Synchronize");
        auto core = mCore;
        auto world = mWorld;
        if (!core || !world) return;
        std::vector<EntityHandle> snapshot;
        for (auto raw : mLevel->mRegistry.view<ScriptComponent>()) snapshot.push_back(raw);
        // Creation invokes Initialize/Enable. Sort the creation batch by the
        // same type order as frame phases, with scene UUIDs as stable ties.
        std::map<std::pair<std::string, std::string>, int> orders;
        for (auto raw : snapshot)
        {
            const auto& component = mLevel->mRegistry.get<ScriptComponent>(raw);
            const auto key = std::pair{component.Backend, component.Type};
            if (orders.contains(key)) continue;
            int order = 0;
            try
            {
                if (auto backend = ScriptBackendRegistry::Find(component.Backend))
                    for (const auto& type : backend->Types()) if (type.Id == component.Type) { order = type.ExecutionOrder; break; }
            }
            catch (...) { } // The normal registration path below reports this failure.
            orders.emplace(key, order);
        }
        std::sort(snapshot.begin(), snapshot.end(), [&](auto left, auto right) {
            const auto& a = mLevel->mRegistry.get<ScriptComponent>(left);
            const auto& b = mLevel->mRegistry.get<ScriptComponent>(right);
            return std::tuple{orders.at({a.Backend, a.Type}), a.Type, Entity(left, mLevel).UUID().str()} <
                std::tuple{orders.at({b.Backend, b.Type}), b.Type, Entity(right, mLevel).UUID().str()};
        });
        for (const auto raw : snapshot)
        {
            if (core != mCore) break;
            auto* component = mLevel->mRegistry.try_get<ScriptComponent>(raw);
            if (!component) continue;
            const ScriptComponent saved = *component;
            auto it = mOwners.find(raw);
            if (it != mOwners.end() && (it->second.Backend != saved.Backend || it->second.Type != saved.Type))
            {
                OnComponentRemoving(mLevel->mRegistry, raw);
                it = mOwners.end();
            }
            if (it == mOwners.end())
            {
                mOwners.emplace(raw, Owner{0, saved.Backend, saved.Type, false, saved.Fields});
                if (saved.Type.empty()) continue;
                ScriptInstanceHandle instance = 0;
                try
                {
                    if (!mRegistered.contains(saved.Backend))
                    {
                        if (auto backend = ScriptBackendRegistry::Find(saved.Backend))
                        {
                            core->Register(std::move(backend));
                            mRegistered.insert(saved.Backend);
                        }
                    }
                    instance = core->Add(saved.Backend, saved.Type,
                        world->Bind(Entity{raw, mLevel}), DecodeScriptFields(saved.Fields), saved.Enabled);
                }
                catch (const std::exception& error)
                {
                    HE_CORE_ERROR("[Script] entity={0}, backend={1}, type={2}: {3}",
                        static_cast<std::uint32_t>(raw), saved.Backend, saved.Type, error.what());
                }
                // Creation can delete the entity/component or stop the scene.
                if (auto owner = mOwners.find(raw); core == mCore && owner != mOwners.end()) owner->second.Instance = instance;
                else if (instance) core->Remove(instance);
            }
            else if (it->second.Instance && !it->second.FieldFault)
            {
                const auto instance = it->second.Instance;
                if (saved.Fields != it->second.LastFields && core->Contains(instance))
                {
                    it->second.LastFields = saved.Fields;
                    try { core->RestoreFields(instance, DecodeScriptFields(saved.Fields)); }
                    catch (const std::exception& error) { HE_CORE_ERROR("[Script] invalid edited fields: {0}", error.what()); }
                }
                core->SetEnabled(instance, saved.Enabled);
            }
        }
        world->FlushCommands();
    }

    void ScriptSystem::QueueContact(EntityHandle receiver, EntityHandle other, std::uint32_t phase, bool trigger,
        const glm::vec3& point, const glm::vec3& normal)
    {
        if (!mWorld || !mCore || !mLevel->mRegistry.valid(receiver) ||
            !mLevel->mRegistry.all_of<ScriptComponent>(receiver)) return;
        if (mContacts.size() >= 32768) throw std::runtime_error("Script physics event queue exceeded 32768 events.");
        const auto otherHandle = mLevel->mRegistry.valid(other) ? mWorld->Bind(Entity{other, mLevel}) : 0;
        mContacts.push_back({receiver, {otherHandle, phase, trigger, {point.x, point.y, point.z}, {normal.x, normal.y, normal.z}}});
    }

    void ScriptSystem::OnUpdateRuntime(Timestep ts)
    {
        HE_PROFILE_SECTION("Script.UpdateAndSnapshot");
        static const bool bTimeScripts = [] {
            const char* setting = std::getenv("HE_RUNTIME_SYSTEM_TIMINGS");
            return setting && setting[0] == '2' && setting[1] == '\0';
        }();
        const auto start = bTimeScripts ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        Synchronize();
        const auto synchronized = bTimeScripts ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        auto core = mCore;
        auto world = mWorld;
        if (!core || !world) return;
        if (core->IsPaused()) core->Step(ts.GetSeconds());
        else core->Update(ts.GetSeconds());
        const auto updated = bTimeScripts ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        auto contacts = std::move(mContacts);
        mContacts.clear();
        for (const auto& pending : contacts)
        {
            if (core != mCore) break;
            const auto owner = mOwners.find(pending.Receiver);
            if (owner != mOwners.end() && owner->second.Instance && !owner->second.FieldFault)
                core->Contact(owner->second.Instance, pending.Event, core->IsPaused());
        }
        world->FlushCommands();
        const auto flushed = bTimeScripts ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        const auto snapshot = mOwners;
        HE_PROFILE_SECTION("Script.Snapshot");
        for (const auto& [raw, owner] : snapshot)
        {
            if (core != mCore) break;
            if (!owner.Instance || owner.FieldFault || !core->Contains(owner.Instance)) continue;
            try
            {
                const auto fields = EncodeScriptFields(core->Save(owner.Instance));
                const auto current = mOwners.find(raw);
                if (core == mCore && current != mOwners.end() && current->second.Instance == owner.Instance)
                    if (auto* component = mLevel->mRegistry.try_get<ScriptComponent>(raw))
                    {
                        component->Fields = fields;
                        current->second.LastFields = fields;
                    }
            }
            catch (const std::exception& error)
            {
                if (auto current = mOwners.find(raw); current != mOwners.end()) current->second.FieldFault = true;
                core->SetEnabled(owner.Instance, false);
                HE_CORE_ERROR("[Script] field snapshot failed for instance={0}: {1}", owner.Instance, error.what());
            }
        }
        if (bTimeScripts)
        {
            const auto end = std::chrono::steady_clock::now();
            const double totalMilliseconds = std::chrono::duration<double, std::milli>(
                end - start).count();
            if (totalMilliseconds > 5.0)
                HE_CORE_WARN("[ScriptSystemTiming] frame={} totalMs={} syncMs={} updateMs={} contactsAndFlushMs={} snapshotMs={} owners={}",
                    SimulationClock::GetInstance().GetFrameNumber(), totalMilliseconds,
                    std::chrono::duration<double, std::milli>(synchronized - start).count(),
                    std::chrono::duration<double, std::milli>(updated - synchronized).count(),
                    std::chrono::duration<double, std::milli>(flushed - updated).count(),
                    std::chrono::duration<double, std::milli>(end - flushed).count(), mOwners.size());
        }
    }
}
