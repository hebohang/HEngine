#include "Hepch.h"
#include "Runtime/EcsFramework/Component/Physics/ThreeD/Rigidbody3DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/TwoD/Rigidbody2DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/TwoD/BoxCollider2DComponent.h"
#include "Runtime/EcsFramework/Component/Script/ScriptComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/System/Physics/PhysicSystem3D.h"
#include "Runtime/EcsFramework/System/Physics/PhysicSystem2D.h"
#include "Runtime/EcsFramework/System/Script/ScriptSystem.h"
#include "Runtime/Scripting/ScriptBackendRegistry.h"
#include "Runtime/Platform/Scripting/ScriptPlatformServices.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"
#if HE_ENABLE_PHYSICS_3D
#include "Runtime/Platform/PhysicsCookedMesh.h"
#endif

#include <stdexcept>
#include <thread>

namespace HEngine::Tests
{
    void PhysicsRayQueryContract()
    {
#if HE_ENABLE_PHYSICS_3D
        const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
        const bool previous3D = ModeManager::b3DMode;
        const bool previousDebug = ModeManager::bShowPhysicsColliders;
        ModeManager::b3DMode = true; ModeManager::bShowPhysicsColliders = false;
        struct RestoreMode
        {
            bool Mode, Debug;
            ~RestoreMode() { ModeManager::b3DMode = Mode; ModeManager::bShowPhysicsColliders = Debug; }
        } restore{previous3D, previousDebug};
        Level level;
        ScriptWorld world(level);
        auto context = level.CreateEntity("Ray owner");
        const auto owner = world.Bind(context);
        const auto box = [&](const char* name, glm::vec3 position, glm::vec3 scale, bool kinematic) {
            auto entity = level.CreateEntity(name);
            entity.GetComponent<TransformComponent>().Translation = position;
            entity.GetComponent<TransformComponent>().Scale = scale;
            entity.AddComponent<Rigidbody3DComponent>().Type = kinematic
                ? Rigidbody3DComponent::Body3DType::Kinematic : Rigidbody3DComponent::Body3DType::Static;
            return world.Bind(entity);
        };
        const auto source = box("Source actor", {0, 1, 0}, {1, 1, 1}, true);
        const auto target = box("Target actor", {3, 1, 0}, {1, 1, 1}, true);
        const auto wall = box("World wall", {6, 1, 0}, {1, 2, 2}, false);
        box("Ground", {0, -.5f, 0}, {10, 1, 10}, false);
        const auto shelf = box("Overhead shelf", {0, 2, 0}, {2, .5f, 2}, false);
        auto* physics = level.TryGetSystem<PhysicSystem3D>();
        require(physics != nullptr, "Ray queries need a real physics world.");
        struct Stop { PhysicSystem3D* Physics; ~Stop() { Physics->OnRuntimeStop(); } } stop{physics};
        physics->OnRuntiemStart();
        const Json ray{{"origin", {0, 1, 0}}, {"direction", {1, 0, 0}}, {"distance", 10}};
        const auto cast = [&](Json args) {
            return Json::parse(ScriptPlatformServices::Execute(owner, "physics.raycastDistance", args.dump())).get<float>();
        };
        require(cast(ray) < .01f, "Unfiltered ray should hit its source actor.");
        auto single = ray; single["entity"] = source;
        require(std::abs(cast(single) - 2.5f) < .01f, "Legacy single-entity exclusion must remain compatible.");
        auto multiple = ray; multiple["entities"] = Json::array({source, target});
        require(std::abs(cast(multiple) - 5.5f) < .01f,
            "Excluding source and target must still hit the intervening world wall.");
        multiple["entities"].push_back(wall);
        require(cast(multiple) == 10, "A ray without remaining colliders must return its requested distance.");
        const auto rejected = [&](const Json& args) {
            try { (void)cast(args); } catch (const std::exception&) { return true; }
            return false;
        };
        auto invalid = ray; invalid["entities"] = Json::array();
        for (int index = 0; index < 33; ++index) invalid["entities"].push_back(source);
        require(rejected(invalid), "Ray ignore arrays above 32 entries must be rejected.");
        invalid["entities"] = Json::array({0});
        require(rejected(invalid), "Default entities are not valid entries in a ray ignore array.");
        invalid["entities"] = Json::array({1.5f});
        require(rejected(invalid), "Fractional entity handles must be rejected.");
        Level otherLevel;
        ScriptWorld otherWorld(otherLevel);
        invalid["entities"] = Json::array({otherWorld.Bind(otherLevel.CreateEntity("Foreign actor"))});
        require(rejected(invalid), "Cross-scene ray exclusions must be rejected.");
        auto staleEntity = level.CreateEntity("Retired actor");
        const auto stale = world.Bind(staleEntity);
        level.DestroyEntity(staleEntity);
        invalid["entities"] = Json::array({stale});
        require(rejected(invalid), "Retired ray exclusion handles must be rejected.");
        Json floor{{"position", {0, 0, 0}}, {"above", 3}, {"below", 2}};
        const auto findFloor = [&](const Json& args) {
            return Json::parse(ScriptPlatformServices::Execute(owner, "physics.floor", args.dump()));
        };
        const auto overhead = findFloor(floor);
        require(overhead.at("found").get<bool>() && std::abs(overhead.at("height").get<float>() - 2.25f) < .01f,
            "Floor query must retain its static closest-surface semantics.");
        floor["entity"] = shelf;
        const auto ground = findFloor(floor);
        require(ground.at("found").get<bool>() && std::abs(ground.at("height").get<float>()) < .01f,
            "Floor query must ignore the selected body and find the actual ground.");
        const auto setEnabled = [&](std::uint64_t entity, bool enabled) {
            return Json::parse(ScriptPlatformServices::Execute(owner, "physics.bodyEnabled",
                Json{{"entity", entity}, {"enabled", enabled}}.dump())).get<bool>();
        };
        auto targetEntity = ScriptWorld::Resolve(target);
        const auto targetBody = targetEntity.GetComponent<Rigidbody3DComponent>().RuntimeBody;
        const auto bodyCount = physics->GetPhysicsSystem()->GetNumBodies();
        require(setEnabled(target, false) && setEnabled(target, false), "Disabling a body must be idempotent.");
        require(std::abs(cast(single) - 5.5f) < .01f, "Disabled actor must no longer obstruct ray queries.");
        physics->OnUpdateRuntime(Timestep(1.0f / 60));
        require(targetEntity.GetComponent<Rigidbody3DComponent>().RuntimeBody == targetBody &&
            physics->GetPhysicsSystem()->GetNumBodies() == bodyCount && std::abs(cast(single) - 5.5f) < .01f,
            "A disabled body must not be recreated or leaked on the next simulation tick.");
        targetEntity.GetComponent<TransformComponent>().Translation.x = 4;
        require(setEnabled(target, true) && setEnabled(target, true), "Enabling a body must be idempotent.");
        require(std::abs(cast(single) - 3.5f) < .01f,
            "A reset actor must rejoin queries at its current ECS world transform.");
        require(setEnabled(target, false), "Occlusion fixture target should deactivate.");
        targetEntity.GetComponent<TransformComponent>().Translation.x = 8;
        require(setEnabled(target, true), "Occlusion fixture target should reactivate.");
        auto lineOfSight = ray;
        lineOfSight["distance"] = 8;
        lineOfSight["entities"] = Json::array({source, target});
        require(std::abs(cast(lineOfSight) - 5.5f) < .01f,
            "Ignoring both endpoint actors must never remove a wall between them.");
        require(!setEnabled(owner, true), "Enabling an entity with no rigid body must report false.");
        require(setEnabled(target, false), "Fixture body should deactivate before removal.");
        targetEntity.RemoveComponent<Rigidbody3DComponent>();
        auto& staleBody = targetEntity.AddComponent<Rigidbody3DComponent>();
        staleBody.RuntimeBody = targetBody;
        require(!setEnabled(target, true), "A destroyed BodyID must be rejected before AddBody.");
        staleBody.RuntimeBody = nullptr;
        targetEntity.RemoveComponent<Rigidbody3DComponent>();
        bool staleRejected = false;
        try { (void)setEnabled(stale, true); } catch (const std::invalid_argument&) { staleRejected = true; }
        require(staleRejected, "Body enable must reject a retired entity handle.");
#else
        throw std::runtime_error("PhysicsRayQuery requires HE_ENABLE_PHYSICS_3D.");
#endif
    }

    namespace
    {
        struct ContactBackend final : IScriptBackend, IScriptPhysicsBackend
        {
            ScriptBackendDescriptor Info{"test.contacts", 1, {ScriptPlatform::Windows}};
            std::vector<ScriptType> ScriptTypes{{"receiver", "", {}, {}}};
            ScriptInstanceHandle Next = 1;
            std::vector<ScriptContactEvent> Events;
            std::thread::id Thread = std::this_thread::get_id();
            bool ThrowContact = false;
            int Disabled = 0;
            const ScriptBackendDescriptor& Descriptor() const override { return Info; }
            std::span<const ScriptType> Types() const override { return ScriptTypes; }
            ScriptInstanceHandle Create(const std::string&, ScriptEntityHandle) override { return Next++; }
            void Write(ScriptInstanceHandle, const std::string&, const ScriptValue&) override {}
            ScriptValue Read(ScriptInstanceHandle, const std::string&) override { return false; }
            void Invoke(ScriptInstanceHandle, ScriptCallback callback, double) override { if (callback == ScriptCallback::Disable) ++Disabled; }
            void Release(ScriptInstanceHandle) noexcept override {}
            void Contact(ScriptInstanceHandle, const ScriptContactEvent& contact) override
            {
                if (std::this_thread::get_id() != Thread) throw std::runtime_error("Physics event ran on a worker thread.");
                if (ThrowContact) throw std::runtime_error("Injected physics callback failure");
                Events.push_back(contact);
            }
        };
    }

    void PhysicsScriptContact2DContract()
    {
#if HE_ENABLE_PHYSICS_2D
        auto require = [](bool condition, const char* message) { if (!condition) throw std::runtime_error(message); };
        const bool previous3D = ModeManager::b3DMode;
        const bool previousDebug = ModeManager::bShowPhysicsColliders;
        ModeManager::b3DMode = false; ModeManager::bShowPhysicsColliders = false;
        struct Cleanup
        {
            bool Mode, Debug;
            ~Cleanup() { ScriptBackendRegistry::Unregister("test.contacts"); ModeManager::b3DMode = Mode; ModeManager::bShowPhysicsColliders = Debug; }
        } cleanup{previous3D, previousDebug};
        auto backend = std::make_shared<ContactBackend>();
        ScriptBackendRegistry::Register(backend);
        for (bool trigger : {false, true})
        {
            auto level = CreateRef<Level>();
            auto floor = level->CreateEntity("2D contact floor");
            floor.GetComponent<TransformComponent>().Translation.y = -.5f;
            floor.GetComponent<TransformComponent>().Scale = {10, 1, 1};
            floor.AddComponent<Rigidbody2DComponent>();
            floor.AddComponent<BoxCollider2DComponent>().IsTrigger = trigger;
            auto actor = level->CreateEntity("2D contact actor");
            actor.GetComponent<TransformComponent>().Translation.y = .45f;
            auto& body = actor.AddComponent<Rigidbody2DComponent>();
            body.Type = Rigidbody2DComponent::BodyType::Dynamic;
            actor.AddComponent<BoxCollider2DComponent>();
            auto& script = actor.AddComponent<ScriptComponent>();
            script.Backend = "test.contacts"; script.Type = "receiver";
            auto* physics = level->TryGetSystem<PhysicSystem2D>();
            auto* scripts = level->TryGetSystem<ScriptSystem>();
            require(physics && scripts, "2D scene did not register required systems.");
            struct Stop { PhysicSystem2D* Physics; ScriptSystem* Scripts; ~Stop() { Scripts->OnRuntimeStop(); Physics->OnRuntimeStop(); } } stop{physics, scripts};
            backend->Events.clear(); physics->OnRuntiemStart(); scripts->OnRuntiemStart();
            for (int frame = 0; frame < 3; ++frame)
            { physics->OnUpdateRuntime(Timestep(1.0f/60)); scripts->OnUpdateRuntime(Timestep(1.0f/60)); }
            require(backend->Events.size() >= 2 && backend->Events.front().Phase == 0 && backend->Events[1].Phase == 1,
                "Real Box2D contacts did not deliver Enter then Stay.");
            require(backend->Events.front().Trigger == trigger && bool(ScriptWorld::Resolve(backend->Events.front().Other)),
                "2D contact trigger kind or entity lifetime was lost.");
            require(physics->SetBodyLinearVelocity(body.RuntimeBody, {0, 8}), "Could not separate 2D bodies.");
            glm::vec2 velocity2D;
            require(physics->GetBodyLinearVelocity(body.RuntimeBody, velocity2D) && velocity2D.y == 8,
                "2D body velocity query lost its value.");
            require(physics->ApplyBodyForce(body.RuntimeBody, {0, 1}, true) &&
                physics->GetBodyLinearVelocity(body.RuntimeBody, velocity2D) && velocity2D.y > 8,
                "2D impulse did not change the actual body's momentum.");
            require(physics->ApplyBodyForce(body.RuntimeBody, {0, 1}, false) &&
                !physics->ApplyBodyForce(floor.GetComponent<Rigidbody2DComponent>().RuntimeBody, {0, 1}, true),
                "2D force routing or static-body rejection failed.");
            for (int frame = 0; frame < 30; ++frame)
            { physics->OnUpdateRuntime(Timestep(1.0f/60)); scripts->OnUpdateRuntime(Timestep(1.0f/60)); }
            bool exitSeen = false;
            for (const auto& event : backend->Events) exitSeen |= event.Phase == 2 && event.Trigger == trigger;
            require(exitSeen, "Separating Box2D bodies failed to deliver Exit.");
            auto* previousBody = body.RuntimeBody;
            actor.RemoveComponent<Rigidbody2DComponent>();
            require(!physics->SetBodyLinearVelocity(previousBody, {0, 1}), "Removed 2D component left a live body.");
            require(!physics->ApplyBodyForce(previousBody, {0, 1}, true) && !physics->GetBodyLinearVelocity(previousBody, velocity2D),
                "Removed 2D body accepted force or velocity queries.");
            actor.AddComponent<Rigidbody2DComponent>().Type = Rigidbody2DComponent::BodyType::Dynamic;
            physics->OnUpdateRuntime(Timestep(1.0f/60));
            auto* replacement = actor.GetComponent<Rigidbody2DComponent>().RuntimeBody;
            require(replacement && physics->SetBodyLinearVelocity(replacement, {0, 1}), "Runtime 2D body was not created.");
            level->DestroyEntity(actor);
            require(!physics->SetBodyLinearVelocity(replacement, {0, 1}), "Destroyed entity left a 2D collision body.");
            physics->OnRuntimeStop(); physics->OnRuntiemStart();
            require(floor.GetComponent<Rigidbody2DComponent>().RuntimeBody != nullptr, "Restart kept a stale 2D body pointer.");
        }
#else
        throw std::runtime_error("PhysicsScriptContact2D requires HE_ENABLE_PHYSICS_2D.");
#endif
    }

    void PhysicsScriptContactContract()
    {
#if HE_ENABLE_PHYSICS_3D
        auto require = [](bool condition, const char* message) { if (!condition) throw std::runtime_error(message); };
        // Portable source is a tool input, never permission for runtime ABI fallback.
        for (bool portable : {false, true})
        {
            std::string bytes;
            PhysicsCollisionFormat::AppendLittle(bytes, portable ? PhysicsCollisionFormat::MagicV2 : PhysicsCollisionFormat::MagicV1, 8);
            PhysicsCollisionFormat::AppendLittle(bytes, 0, 8);
            if (portable)
            {
                PhysicsCollisionFormat::AppendLittle(bytes, 3, 8);
                PhysicsCollisionFormat::AppendLittle(bytes, 1, 8);
                bytes.append(36, '\0');
            }
            bytes.push_back('\0');
            bool rejected = false;
            try { (void)RestoreCookedTriangleMesh(std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())); }
            catch (const std::runtime_error& error)
            { rejected = std::string(error.what()).find("ABI mismatch") != std::string::npos; }
            require(rejected, "Runtime accepted foreign-ABI collision or attempted a silent recook.");
        }
        // A collision-only entity must take the cooked-mesh path, including its
        // validation, rather than silently falling back to an unrelated unit box.
        {
            auto level = CreateRef<Level>();
            auto collision = level->CreateEntity("Invalid collision-only triangle mesh");
            collision.AddComponent<Rigidbody3DComponent>().Shape = CollisionShape::TriangleMesh;
            auto* physics = level->TryGetSystem<PhysicSystem3D>();
            require(physics != nullptr, "Triangle collision test requires the physics system.");
            struct Stop { PhysicSystem3D* Physics; ~Stop() { Physics->OnRuntimeStop(); } } stop{physics};
            bool rejected = false;
            try { physics->OnRuntiemStart(); }
            catch (const std::runtime_error& error)
            { rejected = std::string(error.what()).find("CollisionAssetPath") != std::string::npos; }
            require(rejected, "Collision-only triangle mesh bypassed cooked shape validation.");
        }
        const bool previous3D = ModeManager::b3DMode;
        const bool previousDebug = ModeManager::bShowPhysicsColliders;
        ModeManager::b3DMode = true; ModeManager::bShowPhysicsColliders = false;
        struct Cleanup
        {
            bool Mode, Debug;
            ~Cleanup() { ScriptBackendRegistry::Unregister("test.contacts"); ModeManager::b3DMode = Mode; ModeManager::bShowPhysicsColliders = Debug; }
        } cleanup{previous3D, previousDebug};
        auto backend = std::make_shared<ContactBackend>();
        ScriptBackendRegistry::Register(backend);
        for (bool trigger : {false, true})
        {
            auto level = CreateRef<Level>();
            auto floor = level->CreateEntity("Contact floor");
            floor.GetComponent<TransformComponent>().Translation.y = -.5f;
            floor.GetComponent<TransformComponent>().Scale = {10, 1, 10};
            floor.AddComponent<Rigidbody3DComponent>().IsTrigger = trigger;
            auto actor = level->CreateEntity("Contact actor");
            actor.GetComponent<TransformComponent>().Translation.y = .45f;
            auto& body = actor.AddComponent<Rigidbody3DComponent>();
            body.Type = Rigidbody3DComponent::Body3DType::Dynamic;
            body.AllowSleeping = false; body.restitution = 0;
            auto& script = actor.AddComponent<ScriptComponent>();
            script.Backend = "test.contacts"; script.Type = "receiver";
            auto* physics = level->TryGetSystem<PhysicSystem3D>();
            auto* scripts = level->TryGetSystem<ScriptSystem>();
            require(physics && scripts, "3D scene did not register required systems.");
            backend->Events.clear();
            physics->OnRuntiemStart(); scripts->OnRuntiemStart();
            for (int frame = 0; frame < 3; ++frame)
            { physics->OnUpdateRuntime(Timestep(1.0f/60)); scripts->OnUpdateRuntime(Timestep(1.0f/60)); }
            require(backend->Events.size() >= 2 && backend->Events.front().Phase == 0 && backend->Events[1].Phase == 1,
                "Real Jolt contacts did not deliver Enter then Stay.");
            require(backend->Events.front().Trigger == trigger && bool(ScriptWorld::Resolve(backend->Events.front().Other)),
                "Contact trigger kind or other-entity lifetime was lost.");
            require(physics->SetBodyLinearVelocity(body.RuntimeBody, {0, 8, 0}), "Could not move body away from contact.");
            glm::vec3 velocity3D;
            require(physics->GetBodyLinearVelocity(body.RuntimeBody, velocity3D) && velocity3D.y == 8,
                "3D body velocity query lost its value.");
            require(physics->ApplyBodyForce(body.RuntimeBody, {0, 1, 0}, true) &&
                physics->GetBodyLinearVelocity(body.RuntimeBody, velocity3D) && velocity3D.y > 8,
                "3D impulse did not change the actual body's momentum.");
            require(physics->ApplyBodyForce(body.RuntimeBody, {0, 1, 0}, false), "3D force was not applied.");
            for (int frame = 0; frame < 30; ++frame)
            { physics->OnUpdateRuntime(Timestep(1.0f/60)); scripts->OnUpdateRuntime(Timestep(1.0f/60)); }
            bool exitSeen = false;
            for (const auto& event : backend->Events) exitSeen |= event.Phase == 2 && event.Trigger == trigger;
            require(exitSeen, "Separating real bodies failed to deliver Exit.");
            const auto originalBodyCount = physics->GetPhysicsSystem()->GetNumBodies();
            actor.RemoveComponent<Rigidbody3DComponent>();
            require(physics->GetPhysicsSystem()->GetNumBodies() + 1 == originalBodyCount, "Removed 3D component left a collision body.");
            actor.AddComponent<Rigidbody3DComponent>().Type = Rigidbody3DComponent::Body3DType::Dynamic;
            physics->OnUpdateRuntime(Timestep(1.0f/60));
            require(physics->GetPhysicsSystem()->GetNumBodies() == originalBodyCount && actor.GetComponent<Rigidbody3DComponent>().RuntimeBody,
                "Runtime 3D body was not created.");
            level->DestroyEntity(actor);
            require(physics->GetPhysicsSystem()->GetNumBodies() + 1 == originalBodyCount, "Destroyed entity left a 3D collision body.");
            scripts->OnRuntimeStop(); physics->OnRuntimeStop();
        }
        // Same boundary isolates callback exceptions and drains disable exactly once.
        int errors = 0;
        ScriptCore core(ScriptPlatform::Windows, [](auto) { return true; }, [&](auto, const auto&) { ++errors; });
        core.Register(backend);
        const auto instance = core.Add("test.contacts", "receiver", 1, {});
        core.Update(1.0/60);
        const auto contactsBeforePause = backend->Events.size();
        core.SetPaused(true);
        core.Contact(instance, {});
        require(backend->Events.size() == contactsBeforePause, "Paused scene dispatched a non-step contact.");
        core.Contact(instance, {}, true);
        require(backend->Events.size() == contactsBeforePause + 1, "Single step dropped its physics contact.");
        core.SetPaused(false);
        const int disabledBefore = backend->Disabled;
        backend->ThrowContact = true;
        core.Contact(instance, {}); core.Contact(instance, {});
        require(errors == 1 && backend->Disabled == disabledBefore + 1, "Contact exception escaped or faulted instance executed again.");
        core.Stop();
#else
        throw std::runtime_error("PhysicsScriptContact requires HE_ENABLE_PHYSICS_3D.");
#endif
    }
}
