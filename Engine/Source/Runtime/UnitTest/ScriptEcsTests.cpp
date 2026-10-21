#include "ScriptEcsTests.h"
#include "Runtime/EcsFramework/Component/Script/ScriptComponent.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"
#include "Runtime/EcsFramework/Component/Animation/SequenceComponent.h"
#include "Runtime/EcsFramework/Serializer/SceneSerializer.h"
#include "Runtime/EcsFramework/Serializer/SceneEditingService.h"
#include "Runtime/EcsFramework/Serializer/SceneEditingLevel.h"
#include "Runtime/EcsFramework/System/Script/ScriptSystem.h"
#include "Runtime/Scripting/ScriptBackendRegistry.h"
#include "Runtime/Scripting/ScriptFieldsJson.h"
#include "Runtime/Scripting/ScriptEngineApi.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Platform/Scripting/ScriptPlatformServices.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

#include <stdexcept>
#include <chrono>
#include <thread>
#include <utility>

namespace HEngine::Tests
{
    namespace
    {
        void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

        // Contract fixture, intentionally not registered as a production plugin.
        struct Backend final : IScriptBackend
        {
            ScriptBackendDescriptor Info{"test.ecs", 1, {ScriptPlatform::Windows}};
            std::vector<ScriptType> ScriptTypes{{"ecs.counter", "", {}, {{"count", "Count", std::int64_t(0), {}}}}};
            struct State { ScriptEntityHandle Entity; std::int64_t Count = 0; };
            std::map<ScriptInstanceHandle, State> States;
            ScriptInstanceHandle Next = 1;
            ScriptEntityHandle LastEntity = 0;
            int Created = 0, Enabled = 0, Disabled = 0, Destroyed = 0;
            std::int64_t InitialCount = 0;
            bool SelfDestroy = false;
            const ScriptBackendDescriptor& Descriptor() const override { return Info; }
            std::span<const ScriptType> Types() const override { return ScriptTypes; }
            ScriptInstanceHandle Create(const std::string&, ScriptEntityHandle entity) override
            {
                const auto id = Next++;
                States.emplace(id, State{entity});
                LastEntity = entity;
                return id;
            }
            void Write(ScriptInstanceHandle id, const std::string&, const ScriptValue& value) override { States.at(id).Count = std::get<std::int64_t>(value); }
            ScriptValue Read(ScriptInstanceHandle id, const std::string&) override { return States.at(id).Count; }
            void Invoke(ScriptInstanceHandle id, ScriptCallback callback, double) override
            {
                auto& state = States.at(id);
                switch (callback)
                {
                case ScriptCallback::Initialize: ++Created; InitialCount = state.Count; break;
                case ScriptCallback::Enable: ++Enabled; break;
                case ScriptCallback::Disable: ++Disabled; break;
                case ScriptCallback::Destroy: ++Destroyed; break;
                case ScriptCallback::Update:
                    ++state.Count;
                    if (SelfDestroy)
                    {
                        Require(ScriptWorld::RequestDestroy(state.Entity), "Self-destroy command was rejected.");
                        Require(!ScriptWorld::Resolve(state.Entity), "Pending destruction left its handle usable.");
                    }
                    break;
                }
            }
            void Release(ScriptInstanceHandle id) noexcept override { States.erase(id); }
        };
    }

    void ScriptEcsContract()
    {
        struct ResourceFixture
        {
            std::filesystem::path Root = std::filesystem::temp_directory_path() / ("HE ScriptEcs " + UUID().str());
            ResourceFixture()
            {
                std::filesystem::create_directories(Root / "Config");
                std::filesystem::create_directories(Root / "Assets/WeChatSample");
                std::filesystem::create_directories(Root / "EngineContent");
                Utils::SaveJsonToFile(Json{{"Renderer", {{"API", "DX12"}, {"VSync", false}}}}, Root / "Config/BaseEngine.json");
                Utils::SaveJsonToFile(Json{{"Game", Json::object()}}, Root / "Config/Game.json");
                Utils::SaveJsonToFile(Json{{"TagComponent", {{"Tag", "Async prefab fixture"}}}}, Root / "Assets/WeChatSample/WorkflowChild.hprefab");
                ConfigManager::GetInstance().Init(Root, Root / "EngineContent");
            }
            ~ResourceFixture()
            {
                ConfigManager::GetInstance().Clear();
                std::error_code error;
                std::filesystem::remove_all(Root, error);
            }
        } resourceFixture;
        auto& resources = RuntimeResourceRegistry::GetInstance();
        const bool ownResources = !resources.IsInitialized();
        if (ownResources) resources.Init(1);
        try
        {
            auto level = CreateRef<Level>();
            ScriptWorld world(*level);
            const auto owner = world.Bind(level->CreateEntity("Prefab request owner"));
            const auto other = world.Bind(level->CreateEntity("Different owner"));
            auto actor = ScriptWorld::Resolve(owner);
            actor.AddComponent<AnimatorComponent>();
            actor.AddComponent<SequenceComponent>();
            const auto service = [&](const std::string& name, Json args) {
                args["entity"] = owner;
                return Json::parse(ScriptPlatformServices::Execute(owner, name, args.dump()));
            };
            Require(service("animation.control", {{"action", "pause"}}) == true &&
                service("animation.state", Json::object()).at("paused") == true, "Script animation pause did not change runtime state");
            Require(service("animation.control", {{"action", "resume"}}) == true &&
                service("animation.state", Json::object()).at("paused") == false, "Script animation resume did not change runtime state");
            (void)service("animation.control", {{"action", "speed"}, {"value", 0.5f}});
            Require(service("animation.state", Json::object()).at("speed") == 0.5f, "Animation speed was not forwarded");
            (void)service("animation.control", {{"action", "stop"}});
            Require(service("animation.state", Json::object()).at("playing") == false, "Script animation stop did not reset playback");
            Require(service("animation.set", {{"parameter", "missing"}, {"int", 3}}) == false, "Missing animation parameter reported success");
            Require(service("sequence.control", {{"play", true}}) == true &&
                service("sequence.control", {{"pause", true}}) == true &&
                service("sequence.state", Json::object()).at("paused") == true, "Sequence pause was not exposed");
            Require(service("sequence.control", {{"seek", 2.0f}}) == true &&
                actor.GetComponent<SequenceComponent>().SeekTime == 2.0f, "Sequence seek was not queued for the system boundary");
            for (const auto& request : {std::pair{std::string("animation.control"), Json{{"action", "speed"}, {"value", -1}}},
                std::pair{std::string("sequence.control"), Json{{"seek", 6}}},
                std::pair{std::string("animation.parameter"), Json{{"kind", "float"}, {"parameter", "missing"}}},
                std::pair{std::string("settings.write"), Json{{"path", "Config/Test.json"}, {"value", Json::array()}}}})
            {
                bool rejected = false;
                try { (void)service(request.first, request.second); } catch (const std::exception&) { rejected = true; }
                Require(rejected, "Invalid scene service request was accepted");
            }
            (void)service("sequence.control", {{"play", false}});
            Require(service("sequence.control", {{"pause", false}}) == false, "Inactive sequence resumed unexpectedly");
            int releases = 0;
            const auto serviceLease = ScriptWorld::OwnService(owner, "fixture", 123, [&] { ++releases; });
            Require(ScriptWorld::ReadService(owner, "fixture", serviceLease) == 123, "Typed service lease lost its native value");
            bool wrongOwner = false;
            try { (void)ScriptWorld::ReadService(other, "fixture", serviceLease); } catch (const std::invalid_argument&) { wrongOwner = true; }
            Require(wrongOwner, "Typed service accepted a different script owner");
            Require(ScriptWorld::ReleaseService(owner, "fixture", serviceLease) && releases == 1 &&
                !ScriptWorld::ReleaseService(owner, "fixture", serviceLease), "Service disposal must happen exactly once");
            (void)ScriptWorld::OwnService(owner, "fixture", 456, [&] { ++releases; });
            const auto pending = ScriptWorld::PrefabRequest(owner, 0, 0, "Assets/WeChatSample/WorkflowChild.hprefab");
            bool refused = false;
            try { (void)ScriptWorld::PrefabRequest(other, pending, 1, ""); } catch (const std::invalid_argument&) { refused = true; }
            Require(refused, "Resource request accepted a different owner");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!ScriptWorld::PrefabRequest(owner, pending, 1, "") && std::chrono::steady_clock::now() < deadline)
            {
                (void)resources.PumpMainThreadWork();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Require(ScriptWorld::PrefabRequest(owner, pending, 1, "") == 1, "Real asynchronous prefab load timed out");
            const auto instance = ScriptWorld::PrefabRequest(owner, pending, 3, "");
            Require(bool(ScriptWorld::Resolve(instance)), "Loaded prefab failed to instantiate");
            (void)ScriptWorld::PrefabRequest(owner, pending, 2, "");
            refused = false;
            try { (void)ScriptWorld::PrefabRequest(owner, pending, 1, ""); } catch (const std::invalid_argument&) { refused = true; }
            Require(refused, "Disposed prefab request remained callable");
            const auto stopped = ScriptWorld::PrefabRequest(owner, 0, 0, "Assets/WeChatSample/WorkflowChild.hprefab");
            world.Close();
            Require(releases == 2, "Scene teardown leaked a native service lease");
            Require(ScriptWorld::PrefabRequest(owner, stopped, 2, "") == 0, "Scene cleanup was not idempotent");
        }
        catch (...) { if (ownResources) resources.Shutdown(); throw; }
        if (ownResources) resources.Shutdown();
        {
            auto level = CreateRef<Level>();
            ScriptWorld world(*level);
            const auto owner = world.Bind(level->CreateEntity("Typed API owner"));
            ScriptEngineApi::BindThread();
            ScriptAbi::Request request;
            request.Version = ScriptAbi::Version; request.Entity = owner;
            request.Operation = static_cast<std::uint32_t>(ScriptAbi::EngineOperation::ComponentAccess);
            request.Flags = 2;
            Require(ScriptEngineApi::Call(&request) == 0 && request.Result == 0, "missing optional component query");
            request.Flags = 2 | (1 << 8);
            Require(ScriptEngineApi::Call(&request) == 0 && request.Result != 0, "typed sprite creation");
            const auto previous = request.Result;
            request.Flags = 2 | (2 << 8);
            Require(ScriptEngineApi::Call(&request) == 0 && request.Result == 1, "typed sprite removal");
            request.Flags = 2 | (1 << 8);
            Require(ScriptEngineApi::Call(&request) == 0 && request.Result != previous, "removed component view generation reused");
            request.Operation = static_cast<std::uint32_t>(ScriptAbi::EngineOperation::CreateEntity);
            request.Result = 0;
            Require(ScriptEngineApi::Call(&request) == 0 && request.Result != owner && ScriptWorld::Resolve(request.Result), "typed entity creation");
            const auto child = request.Result;
            request.Operation = static_cast<std::uint32_t>(ScriptAbi::EngineOperation::Hierarchy);
            request.Entity = child; request.Result = owner; request.Flags = 1;
            Require(ScriptEngineApi::Call(&request) == 0, "typed hierarchy parent write");
            request.Flags = 0;
            Require(ScriptEngineApi::Call(&request) == 0 && request.Result == owner, "typed hierarchy parent read");
            auto originalOwner = ScriptWorld::Resolve(owner);
            auto originalChild = ScriptWorld::Resolve(child);
            originalOwner.AddComponent<ScriptComponent>().Fields = EncodeScriptFields({{"child", ScriptEntityReference{originalChild.UUID().str()}}});
            originalChild.AddComponent<ScriptComponent>().Fields = EncodeScriptFields({{"parent", ScriptEntityReference{originalOwner.UUID().str()}}});
            request.Operation = static_cast<std::uint32_t>(ScriptAbi::EngineOperation::CreateEntity);
            request.Entity = owner; request.Result = owner;
            Require(ScriptEngineApi::Call(&request) == 0, "typed hierarchy instantiation failed");
            auto clone = ScriptWorld::Resolve(request.Result);
            const auto clonedChildren = level->GetChildren(clone);
            Require(clonedChildren.size() == 1, "Instantiation lost hierarchy children");
            auto clonedChild = clonedChildren.front();
            Require(std::get<ScriptEntityReference>(DecodeScriptFields(clone.GetComponent<ScriptComponent>().Fields).at("child")).Id == clonedChild.UUID().str(), "Clone retained source child reference");
            Require(std::get<ScriptEntityReference>(DecodeScriptFields(clonedChild.GetComponent<ScriptComponent>().Fields).at("parent")).Id == clone.UUID().str(), "Clone retained source parent reference");
            const auto scriptCount = [&] { std::size_t count = 0; for (auto raw : level->GetAllEntitiesWith<ScriptComponent>()) { (void)raw; ++count; } return count; };
            const auto beforeFailure = scriptCount();
            originalOwner.GetComponent<ScriptComponent>().Fields = "invalid persisted payload";
            request.Result = owner;
            Require(ScriptEngineApi::Call(&request) != 0 && scriptCount() == beforeFailure, "Failed instantiation left partial cloned objects");
            request.Entity = child;
            Require(ScriptWorld::RequestDestroy(child) && !ScriptWorld::Resolve(child), "typed destroy did not invalidate immediately");
            world.FlushCommands();
            request.Operation = static_cast<std::uint32_t>(ScriptAbi::EngineOperation::ComponentAccess); request.Flags = 1;
            Require(ScriptEngineApi::Call(&request) != 0, "component view accepted retired entity");
        }
        {
            auto first = CreateRef<Level>(); auto second = CreateRef<Level>();
            ScriptWorld firstWorld(*first), secondWorld(*second);
            const auto sender = firstWorld.Bind(first->CreateEntity("Event sender"));
            const auto observer = firstWorld.Bind(first->CreateEntity("Event observer"));
            const auto foreign = secondWorld.Bind(second->CreateEntity("Other scene"));
            ScriptWorld::EmitEvent(sender, "score", "12");
            const auto event = ScriptWorld::ReadEvent(observer, "score", 0);
            Require(event.Sequence == 1 && event.Payload == "12", "Same-scene event delivery failed.");
            Require(ScriptWorld::ReadEvent(foreign, "score", 0).Payload.empty(), "Script event crossed a scene boundary.");
            Require(ScriptWorld::ReadEvent(observer, "score", event.Sequence).Payload.empty(), "Script event replayed an old sequence.");
            firstWorld.Close();
            bool rejected = false;
            try { ScriptWorld::EmitEvent(sender, "score", "13"); } catch (const std::invalid_argument&) { rejected = true; }
            Require(rejected, "Stopped scene retained an event context.");
            ScriptWorld restarted(*first);
            const auto fresh = restarted.Bind(first->CreateEntity("Fresh scene run"));
            Require(ScriptWorld::ReadEvent(fresh, "score", 0).Payload.empty(), "A new scene run inherited stale events.");
        }
        auto backend = std::make_shared<Backend>();
        ScriptBackendRegistry::Register(backend);
        try
        {
            auto level = CreateRef<Level>();
            auto entity = level->CreateEntity("ScriptEcs");
            const UUID uuid = entity.UUID();
            auto& component = entity.AddComponent<ScriptComponent>();
            component.Backend = "test.ecs";
            component.Type = "ecs.counter";
            component.Fields = EncodeScriptFields({{"count", std::int64_t(41)}});
            ScriptSystem system(level.get());
            system.OnRuntiemStart();
            Require(backend->Created == 1 && backend->InitialCount == 41 && backend->Enabled == 1,
                "ECS script fields were not restored before Initialize/Enable.");
            system.OnUpdateRuntime(Timestep(0.016f));
            Require(std::get<std::int64_t>(DecodeScriptFields(component.Fields).at("count")) == 42,
                "ECS field state was not captured.");

            SceneSerializer serializer(level);
            const auto saved = serializer.SerializeToJson();
            auto restored = CreateRef<Level>();
            Require(SceneSerializer(restored).DeserializeFromJson(saved), "Script scene did not deserialize.");
            auto rebound = restored->TryGetEntityByUUID(uuid);
            Require(rebound && rebound.GetComponent<ScriptComponent>().Fields == component.Fields,
                "Script type/fields did not survive scene round trip.");
            {
                SceneEditingService author;
                author.Bind(std::filesystem::temp_directory_path(), "he-play-values-contract.he", saved,
                    [&](const Json& values) { ApplySceneEditingValues(restored, values); });
                Json playing = saved;
                playing[uuid.str()]["ScriptComponent"]["Fields"] = EncodeScriptFields({{"count", std::int64_t(99)}, {"unregistered", true}});
                playing[UUID().str()] = saved.at(uuid.str());
                author.SetPlaying(true);
                const auto receipt = author.ApplyPlayScriptFields(playing);
                Require(receipt.at("affectedCount") == 1 && author.GetDocument().size() == saved.size(), "Apply copied runtime-only entities");
                const auto applied = DecodeScriptFields(restored->TryGetEntityByUUID(uuid).GetComponent<ScriptComponent>().Fields);
                Require(std::get<std::int64_t>(applied.at("count")) == 99 && !applied.contains("unregistered"), "Apply copied nonregistered values");
                author.SetPlaying(false);
                const auto control = [&](const char* operation) {
                    return author.Execute(operation, {{"sceneId", author.GetSceneId()}, {"baseRevision", author.GetRevision()}, {"requestId", UUID().str()}});
                };
                Require(control("undo").at("ok") && author.GetDocument() == saved, "Apply Play Values Undo failed");
                Require(control("redo").at("ok") && std::get<std::int64_t>(DecodeScriptFields(restored->TryGetEntityByUUID(uuid).GetComponent<ScriptComponent>().Fields).at("count")) == 99,
                    "Apply Play Values Redo failed");
                const auto execute = [&](Json operations) {
                    return author.Execute("apply", {{"sceneId", author.GetSceneId()}, {"baseRevision", author.GetRevision()},
                        {"requestId", UUID().str()}, {"operations", std::move(operations)}});
                };
                const Json fieldPatch = {{"Fields", EncodeScriptFields({{"count", std::int64_t(17)}})}};
                const Json patchOperation = {{"op", "patch"}, {"entity", uuid.str()}, {"component", "ScriptComponent"}, {"patch", fieldPatch}};
                Require(execute(Json::array({patchOperation})).at("ok"), "Script authoring adapter rejected patch");
                Require(execute(Json::array({{{"op", "duplicate"}, {"entity", uuid.str()}, {"children", "subtree"}}})).at("ok"), "Script authoring adapter rejected duplicate");
                Require(author.GetDocument().size() == saved.size() + 1, "Script duplicate missing");
                Require(control("undo").at("ok") && author.GetDocument().size() == saved.size(), "Script duplicate Undo failed");
                Require(execute(Json::array({{{"op", "remove"}, {"entity", uuid.str()}, {"component", "ScriptComponent"}}})).at("ok"), "Script remove rejected");
                Require(control("undo").at("ok") && restored->TryGetEntityByUUID(uuid).HasComponent<ScriptComponent>(), "Script removal Undo failed");
            }

            component.Enabled = false;
            system.OnUpdateRuntime(Timestep(0.016f));
            Require(backend->Disabled == 1, "Disabling a script failed to invoke Disable.");
            component.Enabled = true;
            system.OnUpdateRuntime(Timestep(0.016f));
            Require(backend->Enabled == 2, "Re-enabling a script failed to invoke Enable.");
            const auto oldHandle = backend->LastEntity;
            backend->SelfDestroy = true;
            system.OnUpdateRuntime(Timestep(0.016f));
            Require(!entity && !ScriptWorld::Resolve(oldHandle) && backend->States.empty(),
                "Self-destruction retained the entity, runtime root, or script handle.");
            auto recycled = level->CreateEntity("Reused slot");
            Require(recycled && !ScriptWorld::Resolve(oldHandle), "Entity slot reuse revived a stale script handle.");
            system.OnRuntimeStop();
            system.OnRuntiemStart();
            system.OnRuntimeStop();
            Require(backend->States.empty(), "Repeated scene runs leaked script roots.");

            // Missing plugin components remain serializable and never execute.
            auto missing = recycled.AddComponent<ScriptComponent>();
            missing.Backend = "missing.plugin";
            missing.Type = "unknown.type";
            missing.Fields = EncodeScriptFields({{"future", std::string("keep me")}});
            recycled.GetComponent<ScriptComponent>() = missing;
            system.OnRuntiemStart();
            system.OnUpdateRuntime(Timestep(0.016f));
            system.OnRuntimeStop();
            Require(recycled.GetComponent<ScriptComponent>().Fields == missing.Fields,
                "Missing plugin handling erased recoverable script data.");
            bool refused = false;
            try { ScriptBackendRegistry::Unregister("test.ecs"); }
            catch (const std::logic_error&) { refused = true; }
            Require(refused, "Registry allowed plugin removal while a backend owner was alive.");
        }
        catch (...)
        {
            backend.reset();
            ScriptBackendRegistry::Unregister("test.ecs");
            throw;
        }
        backend.reset();
        ScriptBackendRegistry::Unregister("test.ecs");
    }
}
