#include "ScriptCore.h"
#include "ScriptFieldsJson.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#include <tuple>

namespace HEngine
{
    namespace
    {
        bool SameFieldType(const ScriptValue& value, const ScriptValue& expected)
        {
            if (value.index() != expected.index()) return false;
            if (const auto* enumeration = std::get_if<ScriptEnum>(&value))
                return enumeration->Type == std::get<ScriptEnum>(expected).Type;
            if (const auto* aggregate = std::get_if<ScriptAggregate>(&value))
                return aggregate->Kind == std::get<ScriptAggregate>(expected).Kind && aggregate->Type == std::get<ScriptAggregate>(expected).Type;
            return true;
        }
    }
    ScriptCore::ScriptCore(ScriptPlatform platform, EntityValidator validator, ErrorSink sink)
        : mPlatform(platform), mValidator(std::move(validator)), mSink(std::move(sink))
    {
        if (!mValidator)
            throw std::invalid_argument("ScriptCore requires an entity validator");
    }

    ScriptCore::~ScriptCore() { Stop(); }

    void ScriptCore::Register(std::shared_ptr<IScriptBackend> backend)
    {
        if (!backend || backend->Descriptor().ApiVersion != 1 || backend->Descriptor().Id.empty())
            throw std::invalid_argument("Invalid script backend descriptor/API version");
        const auto& descriptor = backend->Descriptor();
        if (std::find(descriptor.Platforms.begin(), descriptor.Platforms.end(), mPlatform) == descriptor.Platforms.end())
            throw std::invalid_argument("Script backend does not support this platform: " + descriptor.Id);
        std::set<std::string> typeIds;
        for (const auto& type : backend->Types())
        {
            if (type.Id.empty() || !typeIds.insert(type.Id).second)
                throw std::invalid_argument("Empty or duplicate script type ID");
            std::set<std::string> fieldIds;
            for (const auto& field : type.Fields)
            {
                if (field.Id.empty() || !fieldIds.insert(field.Id).second)
                    throw std::invalid_argument("Empty or duplicate script field ID: " + type.Id);
                for (const auto& former : field.FormerIds)
                    if (former.empty() || !fieldIds.insert(former).second)
                        throw std::invalid_argument("Ambiguous script field migration: " + type.Id);
            }
        }
        if (!mBackends.emplace(descriptor.Id, backend).second)
            throw std::logic_error("Duplicate script backend: " + descriptor.Id);
    }

    void ScriptCore::Unregister(const std::string& backend)
    {
        for (const auto& [id, instance] : mInstances)
            if (instance.Backend->Descriptor().Id == backend)
                throw std::logic_error("Cannot unregister script backend with live instances: " + backend);
        mBackends.erase(backend);
    }

    void ScriptCore::Report(ScriptInstanceHandle id, ScriptErrorKind kind, const std::string& message) noexcept
    {
        try { if (mSink) mSink(id, {kind, message, {}}); }
        catch (...) { /* Diagnostic consumers cannot interrupt cleanup. */ }
    }

    void ScriptCore::Report(ScriptInstanceHandle id, const std::exception& error) noexcept
    {
        if (const auto* script = dynamic_cast<const ScriptException*>(&error))
        {
            try { if (mSink) mSink(id, script->Error()); }
            catch (...) { /* Diagnostic consumers cannot interrupt cleanup. */ }
        }
        else Report(id, ScriptErrorKind::RuntimeFailure, error.what());
    }

    ScriptInstanceHandle ScriptCore::Add(const std::string& backend, const std::string& type,
        ScriptEntityHandle entity, const ScriptFields& fields, bool enabled)
    {
        if (mStopping)
            throw std::logic_error("Cannot create scripts while stopping a scene");
        const auto found = mBackends.find(backend);
        if (found == mBackends.end())
        {
            Report(0, ScriptErrorKind::MissingBackend, "Missing script plugin: " + backend);
            return 0;
        }
        const auto types = found->second->Types();
        const auto typeIt = std::find_if(types.begin(), types.end(), [&](const ScriptType& entry) { return entry.Id == type; });
        if (typeIt == types.end() || !mValidator(entity))
        {
            Report(0, typeIt == types.end() ? ScriptErrorKind::MissingType : ScriptErrorKind::InvalidEntity, type);
            return 0;
        }
        if (mNextId == std::numeric_limits<ScriptInstanceHandle>::max())
            throw std::overflow_error("Script session exhausted instance IDs");
        const auto id = mNextId++;
        Instance instance;
        instance.Backend = found->second;
        instance.Type = *typeIt;
        instance.Entity = entity;
        instance.Preserved = fields;
        instance.WantedEnabled = enabled;
        mInstances.emplace(id, std::move(instance));
        auto& owner = mInstances.at(id);
        ++mCallbackDepth;
        try
        {
            owner.Handle = owner.Backend->Create(type, entity);
            if (!owner.Handle)
                throw std::runtime_error("Backend returned invalid script instance");
            for (const auto& field : owner.Type.Fields)
            {
                auto value = fields.find(field.Id);
                if (value == fields.end())
                    for (const auto& former : field.FormerIds)
                        if ((value = fields.find(former)) != fields.end()) break;
                if (value != fields.end() && !SameFieldType(value->second, field.Default))
                {
                    Report(id, ScriptErrorKind::InvalidField, "Field type changed: " + field.Id);
                    value = fields.end();
                }
                owner.Backend->Write(owner.Handle, field.Id, value == fields.end() ? field.Default : value->second);
            }
        }
        catch (const std::exception& error)
        {
            owner.Faulted = owner.Removing = true;
            Report(id, error);
        }
        catch (...)
        {
            owner.Faulted = owner.Removing = true;
            Report(id, ScriptErrorKind::RuntimeFailure, "Unknown script construction failure");
        }
        --mCallbackDepth;
        Drain();
        return mInstances.contains(id) ? id : 0;
    }

    bool ScriptCore::Call(ScriptInstanceHandle id, ScriptCallback callback, double seconds)
    {
        auto& owner = mInstances.at(id);
        ++mCallbackDepth;
        bool success = true;
        try { owner.Backend->Invoke(owner.Handle, callback, seconds); }
        catch (const std::exception& error)
        {
            success = false;
            if (!owner.Faulted) Report(id, error);
            owner.Faulted = true;
        }
        catch (...)
        {
            success = false;
            if (!owner.Faulted) Report(id, ScriptErrorKind::RuntimeFailure, "Unknown script callback failure");
            owner.Faulted = true;
        }
        --mCallbackDepth;
        return success;
    }

    void ScriptCore::Contact(ScriptInstanceHandle id, const ScriptContactEvent& contact, bool singleStep)
    {
        const auto found = mInstances.find(id);
        if (found == mInstances.end() || (mPaused && !singleStep) || mStopping) return;
        auto& owner = found->second;
        if (!owner.Started || !owner.Enabled || owner.Removing || owner.Faulted || !mValidator(owner.Entity)) return;
        auto* backend = dynamic_cast<IScriptPhysicsBackend*>(owner.Backend.get());
        if (!backend) return;
        ++mCallbackDepth;
        try { backend->Contact(owner.Handle, contact); }
        catch (const std::exception& error) { Report(id, error); owner.Faulted = true; }
        catch (...) { Report(id, ScriptErrorKind::RuntimeFailure, "Unknown physics callback failure"); owner.Faulted = true; }
        --mCallbackDepth;
        Drain();
    }

    void ScriptCore::Drain()
    {
        if (mCallbackDepth || mDraining) return;
        mDraining = true;
        // Snapshot IDs: callbacks may append scripts, but cannot invalidate map nodes.
        const auto ids = OrderedInstances();
        for (const auto id : ids)
        {
            auto& owner = mInstances.at(id);
            if (mStopping && owner.Handle && !owner.UnloadNotified)
            {
                owner.UnloadNotified = true;
                Call(id, ScriptCallback::SceneUnloading);
            }
            if (!mValidator(owner.Entity)) owner.Removing = true;
            if (!owner.Removing && !owner.Faulted && !owner.Initialized)
            {
                owner.Initialized = true;
                Call(id, ScriptCallback::Initialize);
                if (!mValidator(owner.Entity)) owner.Removing = true;
            }
            if (owner.Enabled && (owner.Removing || owner.Faulted || !owner.WantedEnabled))
            {
                owner.Enabled = false;
                Call(id, ScriptCallback::Disable);
            }
            if (!owner.Removing && !owner.Faulted && owner.WantedEnabled && !owner.Enabled)
            {
                owner.Enabled = true;
                Call(id, ScriptCallback::Enable);
                if (!mValidator(owner.Entity)) owner.Removing = true;
            }
            if (owner.Enabled && (owner.Faulted || !owner.WantedEnabled))
            {
                owner.Enabled = false;
                Call(id, ScriptCallback::Disable);
            }
            if (owner.Removing)
            {
                if (owner.Enabled)
                {
                    owner.Enabled = false;
                    Call(id, ScriptCallback::Disable);
                }
                if (owner.Handle)
                {
                    Call(id, ScriptCallback::Destroy);
                    owner.Backend->Release(owner.Handle);
                }
                mInstances.erase(id);
            }
        }
        mDraining = false;
        // A later owner's callback may remove an earlier snapshot entry (or
        // stop the entire scene). Finish those releases before returning.
        if (std::any_of(mInstances.begin(), mInstances.end(), [](const auto& entry) { return entry.second.Removing; }))
            Drain();
        if (mStopping && mInstances.empty()) mStopping = false;
    }

    void ScriptCore::SetEnabled(ScriptInstanceHandle id, bool enabled)
    {
        if (auto it = mInstances.find(id); it != mInstances.end()) it->second.WantedEnabled = enabled;
        Drain();
    }

    void ScriptCore::Remove(ScriptInstanceHandle id)
    {
        if (auto it = mInstances.find(id); it != mInstances.end()) it->second.Removing = true;
        Drain();
    }

    std::vector<ScriptInstanceHandle> ScriptCore::OrderedInstances() const
    {
        std::vector<ScriptInstanceHandle> ids;
        for (const auto& [id, owner] : mInstances) ids.push_back(id);
        std::sort(ids.begin(), ids.end(), [&](auto left, auto right) {
            const auto& a = mInstances.at(left);
            const auto& b = mInstances.at(right);
            return std::tie(a.Type.ExecutionOrder, a.Type.Id, a.Entity, left) <
                std::tie(b.Type.ExecutionOrder, b.Type.Id, b.Entity, right);
        });
        return ids;
    }

    void ScriptCore::RunPhase(ScriptCallback callback, double seconds)
    {
        const auto ids = OrderedInstances();
        for (const auto id : ids)
        {
            auto it = mInstances.find(id);
            if (it == mInstances.end()) continue;
            auto& owner = it->second;
            if (!mValidator(owner.Entity)) owner.Removing = true;
            if (owner.Initialized && owner.Enabled && !owner.Removing && !owner.Faulted)
            {
                if (callback == ScriptCallback::Start)
                {
                    if (!owner.Started) { owner.Started = true; Call(id, callback, seconds); }
                }
                else if (owner.Started) Call(id, callback, seconds);
            }
            Drain();
        }
    }

    void ScriptCore::Advance(double seconds)
    {
        if (mCallbackDepth || mDraining || mAdvancing) throw std::logic_error("Recursive script update");
        if (!std::isfinite(seconds) || seconds < 0) throw std::invalid_argument("Invalid script timestep");
        mAdvancing = true;
        try
        {
            for (const auto& [name, backend] : mBackends)
                if (auto* scheduler = dynamic_cast<IScriptFrameScheduler*>(backend.get())) scheduler->BeginFrame(seconds, mPaused);
            Drain();
            RunPhase(ScriptCallback::Start, 0);
            // Bound catch-up without discarding the remainder of an ordinary frame.
            mFixedAccumulator = std::min(mFixedAccumulator + seconds, 0.25);
            while (mFixedAccumulator + 1e-12 >= FixedSeconds)
            {
                mFixedAccumulator -= FixedSeconds;
                RunPhase(ScriptCallback::FixedUpdate, FixedSeconds);
            }
            RunPhase(ScriptCallback::Update, seconds);
            RunPhase(ScriptCallback::LateUpdate, seconds);
        }
        catch (...) { mAdvancing = false; throw; }
        mAdvancing = false;
    }

    void ScriptCore::Update(double seconds)
    {
        if (!mPaused) Advance(seconds);
    }

    void ScriptCore::Step(double seconds)
    {
        if (!mPaused) throw std::logic_error("Single step requires a paused script scene");
        Advance(seconds);
    }

    void ScriptCore::Notify(ScriptCallback callback)
    {
        if (mCallbackDepth || mDraining) throw std::logic_error("Recursive script notification");
        if (callback != ScriptCallback::Pause && callback != ScriptCallback::Resume &&
            callback != ScriptCallback::ApplicationQuit && callback != ScriptCallback::SceneLoaded &&
            callback != ScriptCallback::SceneUnloading) throw std::invalid_argument("Invalid scene notification");
        for (const auto id : OrderedInstances())
        {
            const auto it = mInstances.find(id);
            if (it == mInstances.end() || it->second.Removing) continue;
            // Teardown notifications still run after an update fault.
            if (!it->second.Faulted || callback == ScriptCallback::ApplicationQuit || callback == ScriptCallback::SceneUnloading)
                Call(id, callback);
            Drain();
        }
    }

    void ScriptCore::SetPaused(bool paused)
    {
        if (mCallbackDepth || mDraining || mAdvancing) throw std::logic_error("Cannot change pause state inside a script phase");
        if (mPaused == paused) return;
        mPaused = paused;
        Notify(paused ? ScriptCallback::Pause : ScriptCallback::Resume);
    }

    void ScriptCore::Stop()
    {
        mPaused = false;
        mFixedAccumulator = 0;
        mStopping = true;
        for (auto& [id, owner] : mInstances) owner.Removing = true;
        Drain();
        if (mInstances.empty()) mStopping = false;
    }

    ScriptFields ScriptCore::Save(ScriptInstanceHandle id)
    {
        if (mCallbackDepth || mDraining) throw std::logic_error("Cannot save inside script callback");
        auto& owner = mInstances.at(id);
        ScriptFields result = owner.Preserved;
        ++mCallbackDepth;
        try
        {
            auto* snapshot = dynamic_cast<IScriptFieldSnapshot*>(owner.Backend.get());
            ScriptFields values;
            if (snapshot)
            {
                values = snapshot->ReadFields(owner.Handle);
                if (values.size() != owner.Type.Fields.size())
                    throw std::runtime_error("Backend snapshot field count mismatch: " + owner.Type.Id);
            }
            for (const auto& field : owner.Type.Fields)
            {
                if (snapshot && !values.contains(field.Id))
                    throw std::runtime_error("Backend snapshot is missing field: " + field.Id);
                auto value = snapshot ? std::move(values.at(field.Id)) : owner.Backend->Read(owner.Handle, field.Id);
                if (!SameFieldType(value, field.Default))
                    throw std::runtime_error("Backend field type mismatch: " + field.Id);
                if (const auto stored = owner.Preserved.find(field.Id); stored != owner.Preserved.end() && !SameFieldType(stored->second, field.Default))
                    continue; // An explicit editor migration must resolve incompatible saved data.
                if (const auto stored = owner.Preserved.find(field.Id); stored != owner.Preserved.end()) value = PreserveUnknownScriptMembers(stored->second, std::move(value));
                result.insert_or_assign(field.Id, std::move(value));
                if (owner.Removing) break;
            }
        }
        catch (...)
        {
            --mCallbackDepth;
            Drain();
            throw;
        }
        --mCallbackDepth;
        Drain();
        return result;
    }

    void ScriptCore::RestoreFields(ScriptInstanceHandle id, const ScriptFields& fields)
    {
        if (mCallbackDepth || mDraining) throw std::logic_error("Cannot edit fields inside a script callback.");
        auto& owner = mInstances.at(id);
        owner.Preserved = fields;
        ++mCallbackDepth;
        try
        {
            for (const auto& field : owner.Type.Fields)
            {
                const auto value = fields.find(field.Id);
                if (value == fields.end()) continue;
                if (!SameFieldType(value->second, field.Default))
                {
                    Report(id, ScriptErrorKind::InvalidField, "Field type changed: " + field.Id);
                    continue;
                }
                owner.Backend->Write(owner.Handle, field.Id, value->second);
                if (owner.Removing) break;
            }
        }
        catch (const std::exception& error)
        {
            if (!owner.Faulted) Report(id, error);
            owner.Faulted = true;
        }
        catch (...)
        {
            if (!owner.Faulted) Report(id, ScriptErrorKind::RuntimeFailure, "Unknown script field setter failure.");
            owner.Faulted = true;
        }
        --mCallbackDepth;
        Drain();
    }
}
