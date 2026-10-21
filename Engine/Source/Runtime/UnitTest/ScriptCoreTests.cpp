#include "Runtime/UnitTest/ScriptCoreTests.h"
#include "Runtime/Scripting/ScriptCore.h"
#include "Runtime/Scripting/ScriptFieldsJson.h"
#include "Runtime/Scripting/ScriptInputContext.h"
#include "Runtime/Scripting/ScriptPlayState.h"

#include <set>
#include <stdexcept>

namespace HEngine::Tests
{
    namespace
    {
        void Require(bool value, const char* message)
        {
            if (!value) throw std::runtime_error(message);
        }

        // Lifecycle fixture only; never used as a language runtime or game backend.
        class Backend : public IScriptBackend
        {
        public:
            ScriptBackendDescriptor Info{"test.fixture", 1, {ScriptPlatform::Windows}};
            std::vector<ScriptType> Registered{{"fixture", "", {}, {{"speed", "Speed", 2.0, {"oldSpeed"}}}}};
            std::map<ScriptInstanceHandle, ScriptFields> Owners;
            std::vector<ScriptCallback> Calls;
            std::function<void(ScriptCallback)> Callback;
            std::function<void()> ReadCallback;
            ScriptInstanceHandle Next = 1;
            bool ThrowCreate = false;
            const ScriptBackendDescriptor& Descriptor() const override { return Info; }
            std::span<const ScriptType> Types() const override { return Registered; }
            ScriptInstanceHandle Create(const std::string&, ScriptEntityHandle) override
            {
                if (ThrowCreate) throw std::runtime_error("create failed");
                const auto id = Next++;
                Owners.emplace(id, ScriptFields{});
                return id;
            }
            void Write(ScriptInstanceHandle id, const std::string& field, const ScriptValue& value) override
            { Owners.at(id).insert_or_assign(field, value); }
            ScriptValue Read(ScriptInstanceHandle id, const std::string& field) override
            {
                const auto value = Owners.at(id).at(field);
                if (ReadCallback) ReadCallback();
                return value;
            }
            void Invoke(ScriptInstanceHandle id, ScriptCallback callback, double) override
            {
                Require(Owners.contains(id), "callback on released instance");
                Calls.push_back(callback);
                if (Callback) Callback(callback);
            }
            void Release(ScriptInstanceHandle id) noexcept override { Owners.erase(id); }
        };
        class SnapshotBackend final : public Backend, public IScriptFieldSnapshot
        {
        public:
            int SnapshotCalls = 0;
            std::function<void(ScriptFields&)> Mutate;
            ScriptFields ReadFields(ScriptInstanceHandle id) override
            {
                ++SnapshotCalls;
                auto fields = Owners.at(id);
                if (Mutate) Mutate(fields);
                return fields;
            }
        };
    }

    void ScriptCoreContract()
    {
        ScriptPlaySession play;
        bool invalidTransition = false;
        try { play.Step(); } catch (const std::logic_error&) { invalidTransition = true; }
        Require(invalidTransition && play.State() == ScriptPlayState::Edit, "illegal Step changed Edit state");
        for (int round = 0; round < 10; ++round)
        {
            play.BeginPlay();
            invalidTransition = false;
            try { play.BeginPlay(); } catch (const std::logic_error&) { invalidTransition = true; }
            Require(invalidTransition, "concurrent Play accepted");
            play.CommitPlay(); play.Pause(); play.Step();
            Require(play.NeedsStep() && play.IsPaused(), "step state contract");
            play.CompleteStep(); play.Resume(); play.BeginStop(); play.CommitStop();
            Require(play.State() == ScriptPlayState::Edit, "Stop failed to restore Edit");
        }
        play.BeginPlay(); play.AbortPlay();
        Require(play.State() == ScriptPlayState::Edit, "failed Play did not roll back");
        const ScriptInputRegion editorInput{true, true, true, 20, 100, 800, 450};
        Require(!editorInput.Contains(400, 80), "Editor toolbar accepted as gameplay input");
        Require(editorInput.Contains(20, 100) && !editorInput.Contains(820, 550), "Viewport bounds are not half-open");
        auto stoppedInput = editorInput;
        stoppedInput.Enabled = false;
        Require(!stoppedInput.Contains(400, 300), "Stopped scene accepted gameplay input");
        ScriptPointerState pointer;
        pointer.Update(true, false); // UI press, then drag into the viewport.
        pointer.Update(true, true);
        Require(pointer.Sequence() == 0 && !pointer.IsPressed(true), "UI-origin drag leaked into gameplay");
        pointer.Update(false, false);
        pointer.Update(true, true);
        pointer.Update(true, true); // Polling the same physical press.
        Require(pointer.Sequence() == 1 && pointer.IsPressed(true), "Gameplay press counted twice");
        Require(!pointer.IsPressed(false), "Pointer held outside viewport remained active");
        pointer.Update(false, false);
        pointer.Update(true, true);
        pointer.Update(false, false); // Complete click between two script frames.
        Require(pointer.Sequence() == 2 && !pointer.IsPressed(true), "Short click between frames was lost");
        const ScriptFields persisted{{"signed", std::int64_t(-9223372036854775807LL)},
            {"text", std::string("script UTF-8")}, {"vector", std::array<float, 3>{1, 2, 3}},
            {"entity", ScriptEntityReference{"stable-entity-id"}},
            {"resource", ScriptResourceReference{"Assets/texture.png"}},
            {"enum", ScriptEnum{"test.enum", 3}}};
        const auto encoded = EncodeScriptFields(persisted);
        Require(EncodeScriptFields(DecodeScriptFields(encoded)) == encoded, "typed field round trip changed payload");
        const auto aggregate = ParseScriptAggregate(R"({"type":"struct","value":{"type":"test.structure","fields":{"retired":{"type":"string","value":"keep"},"target":{"type":"entity","value":"entity-id"},"texture":{"type":"resource","value":"Assets/texture.png"}}}})");
        const auto replacement = ParseScriptAggregate(R"({"type":"struct","value":{"type":"test.structure","fields":{"target":{"type":"entity","value":"new-id"},"texture":{"type":"int64","value":7}}}})");
        const auto merged = std::get<ScriptAggregate>(PreserveUnknownScriptMembers(aggregate, replacement));
        Require(merged.Json.find("keep") != std::string::npos, "unknown structure member lost");
        Require(ScriptReferenceIds(merged, false) == std::vector<std::string>{"new-id"}, "nested entity reference not updated");
        Require(ScriptReferenceIds(merged, true) == std::vector<std::string>{"Assets/texture.png"}, "incompatible structure member overwritten");
        const auto aggregatePayload = EncodeScriptFields({{"structure", merged}});
        Require(EncodeScriptFields(DecodeScriptFields(aggregatePayload)) == aggregatePayload, "schema 2 roundtrip changed payload");
        bool aggregateRejected = false;
        try { ParseScriptAggregate(R"({"type":"list","value":{"type":"test.list","items":[]}})"); }
        catch (const std::exception&) { aggregateRejected = true; }
        Require(aggregateRejected, "sequence without generated prototype accepted");
        const auto sequence = ParseScriptAggregate(R"({"type":"list","value":{"type":"test.entities","items":[{"type":"entity","value":"source"}],"prototype":{"type":"entity","value":"source"}}})");
        const auto remapped = RemapScriptEntityReferences(EncodeScriptFields({{"sequence", sequence}, {"unrelated", std::string("source")}}), {{"source", "clone"}});
        const auto mappedFields = DecodeScriptFields(remapped);
        Require(std::get<ScriptAggregate>(mappedFields.at("sequence")).Json.find("source") == std::string::npos, "Collection item or prototype retained original entity ID");
        Require(std::get<std::string>(mappedFields.at("unrelated")) == "source", "Entity remapping rewrote an arbitrary string");
        bool externalRejected = false;
        try { (void)RemapScriptEntityReferences(EncodeScriptFields({{"sequence", sequence}}), {}, true); }
        catch (const std::invalid_argument&) { externalRejected = true; }
        Require(externalRejected, "Prefab accepted an external script entity reference");
        for (const auto* invalid : {
            R"({"schema":1,"fields":{"x":{"type":"int64","value":18446744073709551615}}})",
            R"({"schema":1,"fields":{"x":{"type":"vec3","value":[1,2]}}})",
            R"({"schema":1,"fields":{"x":{"type":"object","value":{}}}})"})
        {
            bool rejected = false;
            try { DecodeScriptFields(invalid); } catch (const std::exception&) { rejected = true; }
            Require(rejected, "invalid field payload accepted");
        }
        std::set<ScriptEntityHandle> entities{1, 2};
        std::vector<ScriptError> errors;
        ScriptCore core(ScriptPlatform::Windows,
            [&](auto entity) { return entities.contains(entity); },
            [&](auto, const auto& error) { errors.push_back(error); });
        auto backend = std::make_shared<Backend>();
        core.Register(backend);
        backend->Callback = [&](auto callback)
        {
            if (callback == ScriptCallback::Initialize)
                Require(std::get<double>(backend->Owners.begin()->second.at("speed")) == 9.0,
                    "fields must restore before Initialize");
        };
        const auto first = core.Add("test.fixture", "fixture", 1, {{"oldSpeed", 9.0}, {"unknown", std::string("preserved")}});
        Require(first != 0 && backend->Calls == std::vector{ScriptCallback::Initialize, ScriptCallback::Enable}, "startup order");
        Require(core.Save(first).contains("unknown"), "unknown fields lost");
        bool busy = false;
        try { core.Unregister("test.fixture"); } catch (const std::logic_error&) { busy = true; }
        Require(busy, "unregistered live backend");
        backend->Callback = {};
        core.SetEnabled(first, false);
        const auto callsBefore = backend->Calls.size();
        core.Update(0.1);
        Require(backend->Calls.size() == callsBefore, "disabled script updated");
        core.SetEnabled(first, true);
        backend->Callback = [&](auto callback) { if (callback == ScriptCallback::Update) core.Remove(first); };
        core.Update(0.1);
        Require(core.InstanceCount() == 0 && backend->Owners.empty(), "self deletion did not release instance");
        Require(backend->Calls.back() == ScriptCallback::Destroy, "missing destroy callback");

        backend->Callback = {};
        const auto second = core.Add("test.fixture", "fixture", 2, {});
        Require(second > first, "runtime handle reused");
        entities.erase(2);
        core.Update(0.1);
        Require(core.InstanceCount() == 0, "stale entity survived update");
        entities.insert(2);
        const auto third = core.Add("test.fixture", "fixture", 2, {});
        backend->Callback = [](auto callback) { if (callback == ScriptCallback::Update)
            throw ScriptException({ScriptErrorKind::InvalidEntity, "controlled failure", "fixture.Update at fixture:12"}); };
        const auto errorCount = errors.size();
        for (int i = 0; i < 100; ++i) core.Update(0.1);
        Require(errors.size() == errorCount + 1, "script fault log repeated");
        Require(errors.back().Kind == ScriptErrorKind::InvalidEntity && errors.back().Message == "controlled failure" &&
            errors.back().Stack == "fixture.Update at fixture:12", "structured backend diagnostic lost kind/message/stack");
        core.Remove(third);
        backend->Callback = {};
        backend->ThrowCreate = true;
        Require(core.Add("test.fixture", "fixture", 1, {}) == 0, "failed creation published");
        Require(backend->Owners.empty(), "failed creation leaked");
        backend->ThrowCreate = false;
        core.Add("test.fixture", "fixture", 1, {{"speed", std::string("old incompatible type")}});
        Require(errors.back().Kind == ScriptErrorKind::InvalidField, "field mismatch not diagnosed");
        core.Stop();
        Require(core.InstanceCount() == 0 && backend->Owners.empty(), "stop leaked instances");
        core.Add("test.fixture", "fixture", 1, {});
        core.Add("test.fixture", "fixture", 2, {});
        backend->Callback = [&](auto callback) { if (callback == ScriptCallback::Update) core.Stop(); };
        core.Update(0.1);
        Require(core.InstanceCount() == 0 && backend->Owners.empty(), "reentrant scene stop leaked instances");
        backend->Callback = {};
        Require(core.Add("test.fixture", "fixture", 1, {}) != 0, "scene could not restart after callback stop");
        core.Stop();
        const auto readOwner = core.Add("test.fixture", "fixture", 1, {});
        backend->ReadCallback = [&] { core.Remove(readOwner); };
        Require(core.Save(readOwner).contains("speed"), "self-removing getter lost its value");
        Require(core.InstanceCount() == 0 && backend->Owners.empty(), "self-removing getter leaked an instance");
        backend->ReadCallback = {};
        core.Unregister("test.fixture");
        Require(core.Add("test.fixture", "fixture", 1, {}) == 0 && errors.back().Kind == ScriptErrorKind::MissingBackend, "missing plugin not diagnosed");
        auto snapshot = std::make_shared<SnapshotBackend>();
        snapshot->ReadCallback = [] { throw std::runtime_error("snapshot called individual getter"); };
        core.Register(snapshot);
        const auto snapshotOwner = core.Add("test.fixture", "fixture", 1,
            {{"speed", 7.0}, {"unknown", std::string("retain for future plugin")}});
        auto saved = core.Save(snapshotOwner);
        Require(snapshot->SnapshotCalls == 1 && std::get<double>(saved.at("speed")) == 7.0 && saved.contains("unknown"),
            "batch snapshot did not preserve declared and unknown fields");
        for (int failure = 0; failure < 4; ++failure)
        {
            snapshot->Mutate = [failure](ScriptFields& fields)
            {
                if (failure == 0) fields.clear();
                if (failure == 1) fields.emplace("extra", true);
                if (failure == 2) { fields.erase("speed"); fields.emplace("wrongId", 7.0); }
                if (failure == 3) fields.at("speed") = std::string("wrong type");
            };
            bool invalidSnapshot = false;
            try { core.Save(snapshotOwner); } catch (const std::runtime_error&) { invalidSnapshot = true; }
            Require(invalidSnapshot && core.Contains(snapshotOwner), "invalid snapshot was accepted or destroyed its instance");
        }
        snapshot->Mutate = {};
        Require(std::get<double>(core.Save(snapshotOwner).at("speed")) == 7.0, "snapshot failure corrupted later saves");
        core.Stop();
        Require(snapshot->Owners.empty(), "batch snapshot leaked its instance");
        core.Unregister("test.fixture");
        // Frame scheduling is a contract of the public core, not a CLR detail.
        auto phases = std::make_shared<Backend>();
        core.Register(phases);
        for (int round = 0; round < 8; ++round)
        {
            const auto owner = core.Add("test.fixture", "fixture", 1, {});
            phases->Calls.clear();
            core.Update(1.0 / 30.0);
            Require(phases->Calls == std::vector{ScriptCallback::Start, ScriptCallback::FixedUpdate,
                ScriptCallback::FixedUpdate, ScriptCallback::Update, ScriptCallback::LateUpdate}, "frame phase order");
            phases->Calls.clear();
            core.SetPaused(true);
            core.SetPaused(true);
            core.Update(1);
            Require(phases->Calls == std::vector{ScriptCallback::Pause}, "pause was repeated or advanced time");
            core.Step(1.0 / 60.0);
            Require(phases->Calls == std::vector{ScriptCallback::Pause, ScriptCallback::FixedUpdate,
                ScriptCallback::Update, ScriptCallback::LateUpdate}, "paused single step order");
            core.SetPaused(false);
            Require(phases->Calls.back() == ScriptCallback::Resume, "resume notification missing");
            bool invalidStep = false;
            try { core.Step(0); } catch (const std::logic_error&) { invalidStep = true; }
            Require(invalidStep, "unpaused step accepted");
            core.SetEnabled(owner, false);
            core.SetEnabled(owner, true);
            phases->Calls.clear();
            core.Update(0);
            Require(phases->Calls == std::vector{ScriptCallback::Update, ScriptCallback::LateUpdate}, "Start repeated after enable");
            core.Stop();
            Require(phases->Owners.empty() && !core.IsPaused(), "session state leaked across Stop");
        }
        phases->Callback = [&](auto callback) { if (callback == ScriptCallback::Start) core.Stop(); };
        core.Add("test.fixture", "fixture", 1, {});
        phases->Calls.clear();
        core.Update(0.1);
        Require(phases->Calls == std::vector{ScriptCallback::Start, ScriptCallback::SceneUnloading, ScriptCallback::Disable, ScriptCallback::Destroy},
            "Start self-stop reached later phases");
        phases->Callback = {};
        core.Unregister("test.fixture");

        auto early = std::make_shared<Backend>();
        auto late = std::make_shared<Backend>();
        early->Info.Id = "early"; early->Registered.front().ExecutionOrder = -10;
        late->Info.Id = "late"; late->Registered.front().ExecutionOrder = 10;
        core.Register(early); core.Register(late);
        std::vector<int> order;
        early->Callback = [&](auto callback) { if (callback == ScriptCallback::Update) order.push_back(-10); };
        late->Callback = [&](auto callback) { if (callback == ScriptCallback::Update) order.push_back(10); };
        core.Add("late", "fixture", 1, {});
        core.Add("early", "fixture", 2, {});
        core.Update(0);
        Require(order == std::vector{-10, 10}, "type execution order depends on insertion");
        core.Stop();
        core.Unregister("early"); core.Unregister("late");
        ScriptCore web(ScriptPlatform::Web, [](auto) { return true; }, {});
        bool rejected = false;
        try { web.Register(backend); } catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "unsupported platform accepted");
    }
}
